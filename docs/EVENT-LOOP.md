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
