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
