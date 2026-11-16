# Capsicum 可行性评估与 bectl 例外

本文记录 B5 阶段的结论：bectl 为什么继续作为显式例外，以及守护进程进入
Capsicum 能力模式还缺什么。

## 1. 当前执行路径

已经迁到库调用（不再 exec）的部分：

- GUI 只读：数据集/池列表、vdev 树与状态
- 守护进程写操作：数据集 create/destroy/rename/set/snapshot/rollback/clone、
  send/receive、加密密钥 load/unload/change、部分池操作（create/add/remove/
  destroy/export/scrub/trim/online/offline）

仍然通过 `execv` 执行的操作（不完全列表）：

- `bectl.*`（启动环境；见第 4 节）
- `zpool attach/replace/clear/import/upgrade/split/checkpoint/initialize/...`
- `zfs allow/unallow`
- `zfs send -x / -n / 断点续传`、`zfs receive -x`
- 其余未在 `zg-libzfs-exec.c` 中实现的操作

只要还有任何一条路径需要 `execv`，主守护进程就不能 `cap_enter()`。

## 2. 实测结果（FreeBSD 15.1）

程序一：`cap_enter()` 后 `execv("/bin/echo")`

```
execv in capability mode failed: errno=94 (Not permitted in capability mode)
```

程序二：`cap_enter()` 后 `libzfs_init()`

```
cap_enter -> libzfs_init: FAILED (errno=94 Not permitted in capability mode)
open(/dev/zfs) in capability mode: FAILED (errno=94 Not permitted in capability mode)
```

以 root 运行结果相同。

程序三：先打开 `/dev/zfs`、`cap_rights_limit()`，再 `cap_enter()`

```
pre-open /dev/zfs: OK
after pre-open + cap_enter -> libzfs_init: FAILED (errno=94 Not permitted in capability mode)
```

原因：`libzfs_init()` 会**自己重新 open("/dev/zfs")**，不接收调用方已有的 fd。

## 3. 结论

在能力模式内，`open("/dev/zfs")` 与 `execv` 都被禁止。当前架构下：

1. 主守护进程每次请求 `fork + execv` 工具 → 不能 `cap_enter`
2. 即使全部改成 libzfs，libzfs 仍在能力模式内打开 `/dev/zfs` → 仍不能 `cap_enter`
3. 预先打开 fd 并限制权利也没有用，因为 libzfs 不使用它

因此 **B5 无法通过“改调用方式”直接进入 Capsicum**，缺少的是 libzfs 层面的
fd 注入口，或绕过 libzfs 直接 ioctl。

## 4. bectl 例外

FreeBSD 自带 `libbe`（`/lib/libbe.so.1`，头文件 `/usr/include/be.h`），理论上
可以替代大部分 `bectl` 调用。当前仍保留 CLI，原因：

- `libbe` 依赖 **base libzfs.so.4**；本项目实验路径 dlopen 的是 openzfs 的
  **libzfs.so.7**，同时加载两套 libzfs 会带来 ABI/状态冲突风险
- `libbe` 没有 jail 相关 API（`bectl.jail/unjail` 仍要 CLI）
- 创建/销毁/激活 BE 会修改当前系统根克隆与 nextboot，缺少隔离测试环境，
  不适合在没有完整回归的情况下替换
- bectl 操作频率低，保留 CLI 的风险可控，且白名单、PAM、审计不缺失

因此 B5 的“例外处理”是：**显式保留 bectl 为 CLI 例外**，不把它作为
`cap_enter` 的前置条件；后续若启用严格 Capsicum 模式，应在该模式下禁用
bectl，而不是静默回退。

## 5. 进入 Capsicum 的可行路径

| 方案 | 做法 | 代价 | 风险 |
|---|---|---|---|
| A. libzfs fd 注入 | 给 libzfs 增加“使用已有 /dev/zfs fd”的接口；守护进程 open + `cap_rights_limit` 后 `cap_enter` | 需上游改动或维护补丁 | 中 |
| B. 直接 ioctl | 用预先打开的 `/dev/zfs` + `ioctl` 实现全部 zfs/zpool 操作（或 `libzfs_core` 改造） | 很大，几十个 ioctl/nvlist | 高 |
| C. 部分加固 | 不 `cap_enter`，只对连接 socket/std fd 做 `cap_rights_limit`，并收紧权限 | 小 | 低，但不是能力模式 |

在 A/B 完成前，不建议宣称支持 Capsicum；可以先把 C 作为纵深防御。

## 6. 严格模式设想

若后续实现 A/B，可增加构建/运行开关 `capsicum=strict`：

- 禁止所有 `execv` 回退：未实现的操作直接返回“不支持”，不静默降级
- 启动时打开 `/dev/zfs` 并 `cap_rights_limit`
- `cap_enter()` 后再进入 accept 循环
- bectl 相关操作在该模式下禁用（或在独立的不受限 helper 中执行）

该模式下 daemon 的权限面才真正收敛到 `/dev/zfs` 一个 fd。

## 7. 方案 A 已给出补丁：libzfs fd 注入口

fd 注入补丁与测试程序位于 `feature/capsicum-fd-injection` 分支，为 OpenZFS 2.4.2 增加：

```c
libzfs_handle_t *libzfs_init_fd(int fd);
int libzfs_core_init_fd(int fd);
```

调用方先打开 `/dev/zfs`、`cap_rights_limit()`、`cap_enter()`，再调用
`libzfs_init_fd()`；libzfs 内部通过 `F_DUPFD_CLOEXEC` 复制该 fd，不再自己
`open("/dev/zfs")`。`libzfs_core` 也使用同一个注入 fd。

补丁已通过 `patch -p1 --dry-run` 对 2.4.2 源码验证可干净应用；
本机缺少 libtool，尚未完成编译验证。

## 8. 已实施：方案 C（低成本加固）

本轮已落地：

1. **fd 权利限制**（`daemon/zg-server.c`）
   - 监听 socket：`CAP_ACCEPT | CAP_EVENT | CAP_GETSOCKOPT | CAP_LISTEN | CAP_READ | CAP_WRITE`
   - 连接 fd（子进程）：`CAP_READ | CAP_WRITE | CAP_EVENT | CAP_GETSOCKOPT`
   - 注意：监听 fd 必须包含连接所需的 read/write/getsockopt，否则
     `accept()` 出来的 fd 无法再获得这些权利（`getpeereid()` 会报
     "Capabilities insufficient"）
2. **严格模式**（配置 `strict_mode = on`）
   - 未迁移到 libzfs 的操作直接返回“严格模式：该操作尚未迁移到 libzfs”，
     不再 `execv`
   - 自动启用 libzfs 执行层
   - 实测：`zfs.snapshot` 走 libzfs；`bectl.list` 被拒绝

仍未 `cap_enter`：当前 fd 加固只限制单个 fd 的操作面，不构成能力模式沙箱；
真正的 `cap_enter` 仍需第 3 节的 A/B 方案。

## 9. Casper（libcasper）备选方案评估

FreeBSD 自带 libcasper：

```
/usr/include/libcasper.h
/usr/include/casper/{cap_sysctl,cap_pwd,cap_grp,cap_net,cap_dns,...}.h
/usr/lib/libcasper.so
```

现状：

- 没有现成的 `casper_zfs` 服务
- 若自建，需要做一个“ioctl broker”：特权服务进程先打开 `/dev/zfs`，用
  `cap_ioctls_limit()` 限定允许的 ioctl，再通过 Casper channel 接收客户端的
  ioctl 请求并返回结果
- 客户端 libzfs 仍然直接 `open("/dev/zfs")` 并 `ioctl()`；要让它走 Casper，
  仍需修改 libzfs（把 ioctl 换成 channel 调用），工作量不小于 fd 注入
- 额外成本：IPC、请求校验、白名单维护、服务进程自身权限模型

可取之处：`cap_sysctl` 可以无特权读取 `kstat.zfs.misc.arcstats` 等统计，
适合 `system.arcstats` 一类查询，但不解决 libzfs 的写/管理操作。

结论：**fd 注入（方案 A）仍是进入 Capsicum 的最短路径**；Casper 可作为
系统统计类只读查询的补充，不作为 zfs/zpool 管理操作的替代。
