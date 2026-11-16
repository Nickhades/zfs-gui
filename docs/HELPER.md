# 特权 Helper 架构（双擎驱动）

## 1. 目标

主守护进程在 `cap_enter()` 后保持最小权限；需要挂载/卸载、PAM 认证等
无法在能力模式完成的操作，交给一个极简的 root Helper。

```
GUI ──> zfs-guid（能力模式） ──socket──> zfs-helper（root，不沙箱）
                                   └──> /sbin/zfs mount/unmount
```

## 2. 当前实现（feature/capsicum-daemon）

- 新二进制：`zfs-helper`
- 协议：4 字节大端长度 + JSON

```json
请求: {"v":1,"op":"mount","dataset":"tank/data","force":false,"options":""}
应答: {"ok":true,"code":0,"message":""}
```

- 默认 socket：`/var/run/zfs-gui/zfs-helper.sock`，权限 `0600`
- 仅接受 `getpeereid()` 为 `uid == 0` 的对端
- 只执行 `mount` / `unmount`；数据集名通过 `zg_dataset_name_valid()` 校验
- 使用 `fork + execv("/sbin/zfs", argv)`，不经 shell
- 主进程侧客户端：`daemon/zg-helper.c`，在 `capsicum_mode` 下把
  `zfs.mount/zfs.unmount` 路由给 Helper

Helper 协议单独测试通过；预连接池接入后，能力模式内的挂载链路也已打通：

```
capsicum: 已预连接 5 个 Helper 通道
capsicum: libzfs fd 注入完成，/dev/zfs fd=11
已进入 Capsicum 能力模式
mount   code=0 挂载成功
unmount code=0 卸载成功
```

## 3. 预连接池实现

- 主进程在 `cap_enter()` 前建立 5 条 Helper 连接
- 每条 fd 立即 `cap_rights_limit(CAP_READ | CAP_WRITE)`
- 每个 fork 子进程分配一个独占槽位（父进程维护 pid→slot，reap 时释放）
- Helper 改为每个连接 fork 一个处理进程，保持长连接、循环读写 JSON 帧
- 子进程只使用自己的槽位，避免并发 JSON 帧交错

## 4. 曾经的障碍与解决：能力模式内不能新建 socket 连接

接入主守护进程后实测发现：

- Helper socket 已存在、权限正确
- 但主进程 `cap_enter()` 后，子进程调用 `socket()`/`connect()` 到路径
  返回失败，客户端报 `特权 Helper 不可用`
- 这是 Capsicum 的预期行为：路径名查找和新建连接被禁止

因此**不能**在能力模式内按需连接 Helper。

## 5. 设计选择

已采用**预连接 socket 池**：

- 不需要改 Helper 协议
- 单线程 accept + fork 子进程；FreeBSD 允许在能力模式内 fork，
  但禁止 exec（本架构不使用 exec）
- 连接数与并发上限绑定（当前 5 条）

若未来并发 mount 需求增加，可改为父进程 broker 或扩大池。

## 6. 后续路线

1. 实现预连接 socket 池，让 capsicum 下的 mount/unmount 真正跑通
2. `send/receive`：主进程在沙箱外打开文件 fd，通过 **SCM_RIGHTS** 把 fd
   传给 Helper，Helper 直接读写，避免解析路径
3. PAM/密码：全部移入 Helper；主进程不接触 `/etc/shadow`
4. syslog：`cap_enter()` 前 `openlog()` 并写一条日志强制建立连接，
   再用 `cap_rights_limit` 收窄 `/dev/log` fd
5. 高危写操作（create/clone 等触发 mount 的操作）逐步迁移到 Helper

## 7. 安全边界

- Helper 只接受 root 对端，socket 0600
- Helper 不解析复杂参数：数据集名白名单校验，`options` 仅作为 `-o` 参数
- Helper 不跟随用户路径（未来 SCM_RIGHTS 由主进程打开文件）
- 主进程仍保留白名单、参数校验、PAM 结果与审计日志

## 8. SCM_RIGHTS 单元测试（已完成）

`tests/scm_rights_test.c` 覆盖：

- socketpair + `sendmsg` 传递 `/etc/passwd` fd
- 子进程 `recvmsg(MSG_CMSG_CLOEXEC)` 后 `fstat`、读取内容
- 接收 fd 的 `FD_CLOEXEC` 已由内核置位
- 控制缓冲区过短时 `MSG_CTRUNC` 必须置位
- 100 次 fd 传递后 fd 数量不变

运行：

```sh
meson test -C build scm_rights
# 或
./build/scm_rights_test
```

实测：

```
PASS: child received fd and read file
PASS: MSG_CTRUNC set for short cmsg buffer
fd count: before=19 after=19
SCM_RIGHTS: ALL PASS
```

注意：`CMSG_SPACE(4)` 与 `CMSG_SPACE(8)` 在 64 位对齐后可能相同，
“故意截断”的测试必须用**非空但足够短**的控制缓冲区（例如 8 字节），
否则内核根本不会截断。

### 接入 send/receive 的协议草案

1. 主进程在沙箱外打开文件 fd（`O_NOFOLLOW`，0600）
2. 通过 `sendmsg` 发送 1 字节请求类型 + SCM_RIGHTS fd
3. Helper 用 `recvmsg(MSG_CMSG_CLOEXEC)` 接收，检查 `MSG_CTRUNC`
4. Helper 直接对 fd 执行 send/receive 流操作
5. 主进程在 `sendmsg` 失败时自行 `close(fd)`；成功后可立即关闭本地 fd
6. 所有 fd 均带 `O_CLOEXEC`，避免 exec 继承

下一步在 Helper 协议中加入 `send_stream` / `receive_stream` 两种消息。

## 9. send_stream 已打通（Helper 侧）

Helper 新增 `send_stream`：

1. 客户端发送 JSON 帧：`{"op":"send_stream","snapshot":"pool/ds@snap"}`
2. 客户端紧接着用 `sendmsg` 发送 1 字节 + SCM_RIGHTS 文件 fd
3. Helper 用 `recvmsg(MSG_CMSG_CLOEXEC)` 接收，检查 `MSG_CTRUNC`
4. Helper fork 子进程，`dup2(fd, STDOUT)` 后执行 `zfs send snapshot`
5. 完成后回传 `{ok, message}`

实测：

```
请求 send_stream zgtest/data@s1
fd = /tmp/zgstream.zfs（O_WRONLY|O_CREAT|O_TRUNC, 0600）
应答 {'ok': True, 'code': 0}
文件大小 47,400 字节
zfs receive zgtest/recv < /tmp/zgstream.zfs  → 成功
zgtest/recv@s1 出现
```

说明 fd 传递、Helper 写流、流内容有效性全部验证通过。

### 主进程接入的下一个障碍

主守护进程在 `cap_enter()` 后**无法按路径打开输出文件**，因此不能在
`zfs.send` 请求到达时自己 `open(io_path)`。

可行方案：

1. **GUI 打开文件并把 fd 传给守护进程**（推荐）
   - GUI 是普通用户，打开文件只受用户权限约束，不需要 root
   - 客户端协议增加 SCM_RIGHTS：GUI → zfs-guid → Helper
   - 守护进程不再接触路径，路径穿越风险消失
2. Helper 根据路径打开文件
   - 简单，但把用户路径交给 root Helper，需要更严格的校验
3. 主进程预打开目录 fd + openat
   - 只适合固定目录，不适合任意备份路径

建议采用方案 1：下一步给 GUI↔守护进程协议加 fd 传递。

### 尚未实现

- `receive_stream`（方向相反，fd 由 GUI 打开后传给 Helper）
- 取消：客户端断开时 Helper 通过 socket EOF 终止 `zfs send`
- 进度/状态机：目前 Helper 连接同步等待，长流会占用一个连接槽
