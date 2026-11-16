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

Helper 协议单独测试通过：

```
unmount code=0
mount   code=0
zgtest/data on /tmp/zgcap-mnt
```

## 3. 关键障碍：能力模式内不能新建 socket 连接

接入主守护进程后实测发现：

- Helper socket 已存在、权限正确
- 但主进程 `cap_enter()` 后，子进程调用 `socket()`/`connect()` 到路径
  返回失败，客户端报 `特权 Helper 不可用`
- 这是 Capsicum 的预期行为：路径名查找和新建连接被禁止

因此**不能**在能力模式内按需连接 Helper。

## 4. 修正设计：预连接通道

推荐两种方案：

### A. 预连接 socket 池（简单）

1. 主进程在 `cap_enter()` 前建立 N 条到 Helper 的连接
2. 每个 fork 子进程继承连接池
3. 父进程为每个连接分配唯一的“在使用”标记，子进程只用自己的连接
4. 连接断开后由父进程在能力模式外重连（或 Helper 侧支持重新握手）

优点：不需要改 Helper 协议；缺点：连接数与并发上限绑定。

### B. 父进程 broker

1. 父进程保留唯一 Helper 连接
2. 子进程通过继承的 `socketpair` 把请求发给父进程
3. 父进程串行转发到 Helper

优点：连接数固定；缺点：父进程需要多路复用与超时管理。

无论哪种，`zfs-helper` 协议不变。

## 5. 后续路线

1. 实现预连接 socket 池，让 capsicum 下的 mount/unmount 真正跑通
2. `send/receive`：主进程在沙箱外打开文件 fd，通过 **SCM_RIGHTS** 把 fd
   传给 Helper，Helper 直接读写，避免解析路径
3. PAM/密码：全部移入 Helper；主进程不接触 `/etc/shadow`
4. syslog：`cap_enter()` 前 `openlog()` 并写一条日志强制建立连接，
   再用 `cap_rights_limit` 收窄 `/dev/log` fd
5. 高危写操作（create/clone 等触发 mount 的操作）逐步迁移到 Helper

## 6. 安全边界

- Helper 只接受 root 对端，socket 0600
- Helper 不解析复杂参数：数据集名白名单校验，`options` 仅作为 `-o` 参数
- Helper 不跟随用户路径（未来 SCM_RIGHTS 由主进程打开文件）
- 主进程仍保留白名单、参数校验、PAM 结果与审计日志
