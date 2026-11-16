# Daemon kqueue 事件循环（feature/event-loop）

目标：把当前 `fork-per-connection + 同步等待 Helper` 的模型，重构为
`kqueue + task_entry + 非阻塞帧` 的事件驱动模型。

## 1. 为什么必须换

当前 Capsicum 流任务的问题：

- worker 阻塞在 `read(helper_fd)` 上，无法同时读 `client_fd`
- GUI 的 CANCEL 帧只能等当前请求结束后才被看到
- 客户端断开时 worker 可能在 `POLLHUP` 上忙循环
- 一个长流占住一个预连接 Helper 槽位

## 2. 核心数据结构

```c
typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_CANCELLING,
    STATE_DONE,
} task_state_t;

typedef struct {
    uint32_t req_id;
    int client_fd;
    int helper_fd;
    task_state_t state;
    GByteArray *client_in;   /* 半包缓冲 */
    GByteArray *helper_in;
    guint64 bytes_transferred;
    gint64 last_event_ms;
} task_entry_t;
```

任务表：`GHashTable *tasks`（key = req_id），以及 `GHashTable *by_client_fd`。

## 3. kqueue 主循环

```c
kq = kqueue();

/* 监听 socket */
EV_SET(&kev, listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);

/* 每个连接/任务注册 */
EV_SET(&kev, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, task);
EV_SET(&kev, helper_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, task);
/* 写事件按需 EV_ADD/EV_DISABLE */
```

- 所有 socket 建立后立即 `fcntl(fd, F_SETFL, O_NONBLOCK)`
- `kevent()` 返回后只做非阻塞读写
- 定时器用 `EVFILT_TIMER` 做 CANCEL 超时/任务清理
- 信号用 `EVFILT_SIGNAL`（替代 self-pipe）

## 4. 帧解析（半包/粘包）

每个方向维护一个 `GByteArray` 接收缓冲：

1. `read()` 追加到缓冲区（直到 EAGAIN）
2. 循环检查：缓冲区 >= 4 字节 → 读大端长度
3. 缓冲区 >= 4 + len → 取出完整 JSON 帧，`g_byte_array_remove_range`
4. 不完整就保留，等待下一次 EVFILT_READ

发送端同样维护输出缓冲，`write()` 返回 EAGAIN 时保留剩余数据，
注册 EVFILT_WRITE，下次可写时继续。

## 5. CANCEL 状态机

1. client_fd 可读，解析出 `{"op":"cancel","req_id":N}`
2. 查任务表；若 `state == STATE_RUNNING`：
   - 向 helper_fd 写入 CANCEL 帧
   - `state = STATE_CANCELLING`
3. 不立即关闭任何 fd
4. Helper 回 `CANCELED`/`FAILED` 后：
   - 向 client 转发最终帧（若 client 仍连接）
   - 释放 task、关闭 helper_fd、注销 kqueue
5. 若 client_fd 先 EOF：
   - task 标记 orphan
   - 仍等待 Helper 结束，再静默销毁
6. `STATE_CANCELLING` 超时（例如 5 秒）：
   - 关闭 helper_fd，强制清理，记录审计

## 6. 协议状态帧

保持 JSON（暂时不换二进制）：

```json
{"type":"started","req_id":1001}
{"type":"progress","req_id":1001,"bytes":104857600}
{"type":"completed","req_id":1001,"message":"已发送 ..."}
{"type":"failed","req_id":1001,"message":"..."}
{"type":"canceled","req_id":1001}
```

当前实现使用普通最终应答；重构后统一为上述状态帧。

## 7. 迁移步骤

1. 新增 `zg-frame.c`：非阻塞长度前缀帧缓冲（含单元测试）
2. 新增 `zg-task.c`：task_entry 状态机 + kqueue 循环（先只处理 send_stream）
3. server accept 循环改为 kqueue，连接不再 fork；其他操作先沿用同步执行
4. send_stream 走 task 事件循环；CANCEL 精确路由
5. 迁移 receive_stream
6. 最后删除 fork-per-connection 与阻塞 Helper 客户端

## 8. 实现进度

- `daemon/zg-frame.[ch]`：非阻塞长度前缀帧缓冲
  - 读循环遇 EAGAIN 立即返回，不阻塞
  - `g_byte_array_remove_range` 处理粘包，避免无限拼接
  - 单帧上限默认 1MB，超限返回错误（防 OOM）
  - 输出缓冲支持部分写与 EAGAIN 重试
- `tests/frame_buffer_test.c`：半包、粘包、取消帧、超长帧拒绝、部分写，全部 PASS
- `tests/kqueue_frame_test.c`：kqueue + 非阻塞半包解析，全部 PASS
- selftest 已包含两项测试

## 9. 防御性细节清单（必须遵守）

- 所有 socket 建立后 `fcntl(O_NONBLOCK)`
- `close(fd)` 前先 `EV_DELETE` 对应 kevent
- 每个连接维护独立 `ZgFrameBuffer`，不共享
- CANCEL 状态：
  - 收到 cancel → 写 CANCEL 帧 → `STATE_CANCELLING`
  - 收到 Helper 最终帧 → 转发 → 清理
  - client 先 EOF → orphan，等 Helper 结束后静默销毁
  - CANCELLING 超过 10 秒 → 强制清理并审计
- SIGCHLD：
  - 使用 self-pipe，把信号写端注册到 kqueue
  - 可读时 `waitpid(-1, NULL, WNOHANG)` 循环回收
  - 禁止在信号处理函数里 waitpid/做复杂操作
- 超时：`EVFILT_TIMER` 每 5 秒扫描任务表

## 10. 下一步

- `daemon/zg-task.[ch]`：task_entry + kqueue 循环（先 send_stream）
- server 接入事件循环，保留旧路径直到验证通过
- CANCEL 断言：
  1. `pgrep zfs send` 消失
  2. client 收到 `{"type":"canceled","req_id":...}`
  3. Helper 关闭 fd、清理半文件

## 11. kqueue 双工 Echo 验证（已通过）

`tests/kqueue_echo_test.c` 现在覆盖完整的最小 relay：

1. helper 写 started/progress → relay 转发给 client
2. client 写 cancel → relay 转发给 helper
3. helper 写 final → relay 转发给 client

全部 PASS。

### 之前 hang 的真正原因（ktrace 结论）

ktrace 显示：

```
read(3) = 35        # helper 帧长度+JSON
kevent(...)         # 已注册 EVFILT_READ
write(4, 35)        # 转发给 client
```

syscall 层没有卡在 kevent；问题在 mock 拓扑：

- 旧测试把 relay 输出写到 **peer 端 fd**（`client_peer[1]`），而 kqueue 监听的是
  `client_peer[0]`
- 于是 relay 自己写出的数据会出现在被监听的 fd 上，产生“自己发给自己”的事件，
  真正的 client 请求反而被混淆
- 真实 duplex socket 中，relay 应当向**自己这一端的 fd**写（数据流向 peer），
  而不是向 peer fd 写

修正拓扑后：

```
helper_peer[1] --write--> helper_peer[0] (relay read)
relay --write--> client_peer[0] (自己这端) --> client_peer[1] (mock 读)
```

### 另一个坑：半帧只处理一次

`relay_one` 第一版只做一次 `zg_frame_read + pop`；如果只读到长度或半个 JSON，
就直接返回失败。正确做法是：

- `pop == 0` → 继续等下一次 kevent，不要当作错误
- 直到 `pop == 1` 才转发

这对应真实 task 中的“缓冲区保留半包、等待下一次 EVFILT_READ”。

## 12. 下一步

- 把 `zg-task` 的 kqueue 循环按这个已验证的 relay 拓扑重写
- 加入 4 个 socketpair 的 mock（client/helper 各一对，relay 端读写同一 fd）
- mock 通过后再接真实 `zfs.send`，最后加 `pgrep zfs send` 断言

## 13. 真凶：zg_frame_read 用了阻塞 read

探针日志：

```
[task] KEVENT ident=6 filter=-1 flags=0 data=22
[task] READ fd=6 rr=1 buffered=22 errno=0   # rr=EOF，但缓冲区里明明有 22 字节
```

原因：`zg_frame_read()` 内部用 `read()` 循环读取直到 EAGAIN，但 relay fd 仍是阻塞模式。
第一次读到 22 字节后，循环再次 `read()` 直接阻塞，导致：

- 后续 helper 帧无法处理
- client 永远收不到 started/progress
- task 最终在父进程关闭 socket 后才被 EOF 唤醒并退出

修复：`zg_frame_read()` 改用

```c
recv(fd, temp, sizeof(temp), MSG_DONTWAIT)
```

这样即使 fd 没有设置 O_NONBLOCK，也能在读完当前数据后立即返回 AGAIN。
这也解决了 Capsicum 下 `fcntl(F_SETFL)` 被拒绝的问题。

修复后 `tests/kqueue_task_test.c` 全部通过：

```
task sends request+fd
received fd valid
helper started -> client started
helper progress -> client progress
client cancel -> helper cancel
helper final -> client final
task exits ok
```

selftest 已加入 `kqueue_task_test`。

## 14. 真实链路打通：CAP_EVENT 与两段式 fd

真实 cancel 测试连续踩中三个接线问题：

1. **CAP_EVENT**
   Helper 池 fd 只限制了 `CAP_READ|CAP_WRITE`，`kevent(EV_ADD)` 返回 -1，
   errno=93（ENOTCAPABLE）。
   修复：`cap_rights_init(&rights, CAP_READ, CAP_WRITE, CAP_EVENT, CAP_GETSOCKOPT)`。

2. **两段式 fd**
   Helper 先读 JSON 帧，再单独 `recv_stream_fd()`。
   task 必须：
   ```c
   write(helper_fd, length + json);
   sendmsg(helper_fd, 1 字节 + SCM_RIGHTS(fd));
   ```
   不能把 fd 附在 JSON 首帧上。

3. **Helper 关闭前先发最终帧**
   Helper 取消后会先 `send_response` 再关闭连接。task 收到 `EV_EOF` 时必须先
   drain 缓冲区，读取最终帧，否则会误报 "helper eof"。

实测 64MB 随机数据 + cancel：

```
{'type': 'progress', 'req_id': ..., 'bytes': 8403224}
{'ok': False, 'code': 1, 'message': 'zfs send 已取消'}
PASS: zfs send gone
```

`pgrep -f "zfs send ..."` 断言通过。

## 15. receive_stream 复用同一 task

`zg_task_receive_stream()` 与 send 共用同一个 kqueue 事件循环，只改请求帧：

```json
{"op":"receive_stream","target":"pool/recv","force":false,"req_id":N}
```

随后仍是两段式：SCM_RIGHTS 只读 fd 单独发送。

实测 8MB 流恢复到 `zgtest/recv@s1`：

```
{"ok": true, "code": 0, "message": ""}
zfs list -t snapshot: zgtest/recv@s1
```

当前 receive 尚未做 PROGRESS（Helper 侧未加代理计数），只上报最终状态；
后续可在 Helper 读流循环里累加字节并复用 progress 帧。

## 16. receive 进度代理（poll 版）

Helper 不再把源 fd 直接塞给 zfs receive，而是：

```
源 fd --read--> Helper 缓冲区 --write--> pipe --stdin--> zfs receive
```

poll 同时监听：

- control socket（CANCEL）
- 源 fd（POLLIN，缓冲区空时）
- pipe 写端（POLLOUT，有 pending 数据时）

pipe 写端设为 `O_NONBLOCK`，`EAGAIN` 等下一次 POLLOUT；
源 EOF 且 pending 清空后关闭 pipe 写端，zfs receive 才能收尾。

实测 32MB：

```
PROGRESS 8388608
PROGRESS 16777216
PROGRESS 25165824
PROGRESS 33554432
{'ok': True, 'message': '已接收 33682504 字节'}
zfs list: zgtest/recv@s1
```

取消路径：`setpgid(0,0)` + `kill(-pid, SIGTERM)`，关闭 pipe 写端。
