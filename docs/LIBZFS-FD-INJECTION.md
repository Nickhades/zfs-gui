# libzfs fd 注入口（fd injection）

## 1. 目标

让调用方可以：

1. 以 `O_RDWR|O_EXCL|O_CLOEXEC` 打开 `/dev/zfs`
2. 用 `cap_rights_limit(2)` 收窄该 fd 的权利
3. `cap_enter(2)` 进入 Capsicum 能力模式
4. 再初始化 libzfs，并只通过这个 fd 做 ioctl

当前 libzfs 的 `libzfs_init()` 会在内部重新 `open("/dev/zfs")`，在能力模式下
必然失败（`errno=94`）。本补丁增加 fd 注入口解决这一点。

## 2. 补丁

文件：`patches/openzfs-2.4.2-fd-injection.patch`

新增 API：

```c
libzfs_handle_t *libzfs_init_fd(int fd);
int libzfs_core_init_fd(int fd);
```

语义：

- 调用方保留 fd 所有权；libzfs 内部用 `F_DUPFD_CLOEXEC` 复制一份
- `fd < 0` 的行为与原来的 `libzfs_init()` 一致（打开设备）
- `libzfs_init_fd()` 不调用 `libzfs_load_module()`（能力模式下无法加载模块，
  调用方需保证 ZFS 模块已加载）
- `libzfs_core` 也必须使用同一个 fd，否则它仍会自己 open 设备

补丁修改的文件：

| 文件 | 修改 |
|---|---|
| `include/libzfs.h` | 声明 `libzfs_init_fd()` |
| `include/libzfs_core.h` | 声明 `libzfs_core_init_fd()` |
| `lib/libzfs/libzfs_util.c` | 初始化逻辑接受 fd；拆出 `libzfs_init_impl()` |
| `lib/libzfs_core/libzfs_core.c` | `g_fd` 从注入 fd 复制 |
| `lib/libzfs/libzfs_diff.c` | cleanup fd 改为复制 handle fd |
| `lib/libzfs/libzfs_sendrecv.c` | cleanup fd 改为复制 handle fd |

补丁已用 `patch -p1 --dry-run` 对 OpenZFS 2.4.2 源码验证可干净应用。

## 3. 所需 fd 权利

```c
cap_rights_init(&rights, CAP_IOCTL, CAP_FSTAT, CAP_EVENT);
cap_rights_limit(fd, &rights);
```

- `CAP_IOCTL`：所有 ZFS ioctl
- `CAP_FSTAT` / `CAP_EVENT`：部分代码路径会用到
- FreeBSD 上 `dup(2)`/`fcntl(F_DUPFD)` 不需要额外权利，因此没有 `CAP_DUP`

## 4. 构建与测试

```sh
# 1. 取 OpenZFS 2.4.2 源码
git clone https://github.com/openzfs/zfs
cd zfs
git checkout zfs-2.4.2

# 2. 应用补丁
patch -p1 < /path/to/zfs-gui/patches/openzfs-2.4.2-fd-injection.patch

# 3. 构建用户态（FreeBSD）
./autogen.sh
./configure --with-config=user --prefix=/tmp/zfs-fd
gmake -j"$(sysctl -n hw.ncpu)"
gmake install

# 4. 编译并运行测试
cc -O2 -o /tmp/fdtest /path/to/zfs-gui/tools/libzfs_fd_test.c \
   -I/tmp/zfs-fd/include $(pkg-config --libs libzfs)
LD_LIBRARY_PATH=/tmp/zfs-fd/lib /tmp/fdtest
```

预期输出：

```
pre-open /dev/zfs: OK
cap_enter: OK
libzfs_init_fd: OK
pool zroot
...
```

### 已完成的编译与沙箱验证（FreeBSD 15.1）

- 安装 `libtool` 后，对补丁后的 OpenZFS 2.4.2 执行
  `./autogen.sh && ./configure --with-config=user && gmake -j4` 成功
- 构建出的 `libzfs.so.7` 导出 `libzfs_init_fd`，`libzfs_core.so.3` 导出
  `libzfs_core_init_fd`
- `tools/libzfs_fd_test.c` 链接构建产物后运行通过：

```
libzfs_init_fd (pre-cap): OK
cap_enter: OK
sandbox open(/etc/passwd): denied (errno=94)
sandbox execv: denied (errno=94)
zfs_prop_get(used): ... bytes
fd count (operations): PASS
re-init 1/2/3: PASS
RSS delta (100 init/fini): 0 KB
```

重要限制：`libzfs_core` 第一次 ioctl 前会 `sysctl("vfs.zfs.version.ioctl")`，
能力模式禁止 sysctl。因此在 `cap_enter()` 前必须初始化 libzfs 并执行一次
预热 ioctl（测试程序已这样做）；否则会报
`unrecognized zfs ioctl version 0`。

Valgrind 3.27 在本机 FreeBSD 15.1 上启动即 SIGILL，无法用于泄漏检测；
改用 fd 计数 + RSS 烟测（结果见上）。

## 5. 与守护进程集成（后续）

只有在 **strict_mode** 下才可以安全启用：

1. 启动时打开 `/dev/zfs` 并 `cap_rights_limit`
2. `libzfs_init_fd(fd)` 初始化
3. 确认没有 `execv` 回退（严格模式）
4. `cap_enter()` 后进入 accept 循环
5. 未迁移操作在严格模式下直接拒绝；bectl 等显式禁用

届时守护进程的权限面收敛到 `/dev/zfs` 一个受限 fd。

## 6. 注意事项

- fd 必须是 `O_EXCL`，否则 cleanup fd 的语义可能变化
- `libzfs_fini()` 会关闭 libzfs 内部的复制 fd，不会关闭调用方的 fd
- fork 后的子进程会继承 fd；不建议在多个子进程里共享同一个 libzfs handle
- 该 API 需要上游接受或由发行版长期维护补丁
