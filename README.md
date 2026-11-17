# ZFS-GUI for FreeBSD

**FreeBSD + OpenZFS 的 GTK4 图形管理套件**
**A GTK4 ZFS management suite for FreeBSD + OpenZFS**

[简体中文完整文档](README.zh-CN.md) · [English guide](README.en.md)

当前版本：**v1.1.0**（kqueue / Capsicum / SCM_RIGHTS 架构）

---

## 这是什么

ZFS-GUI 把 `zpool` / `zfs` / `bectl` 的能力做成对象驱动的图形界面：
先点存储池、数据集、快照或启动环境，再点操作，不需要记命令参数。

界面进程以普通用户运行；特权操作交给 root 守护进程 `zfs-guid`，
再由独立的特权 Helper `zfs-helper` 完成需要文件描述符的操作。

---

## 新版架构（v1.1.0）

```
┌──────────────────────────────┐
│ zfs-gui  (GTK4, 普通用户)     │
│ 界面 / 表单 / 进度 / 取消     │
└──────────────┬───────────────┘
               │ AF_UNIX, 4 字节长度 + JSON
┌──────────────▼───────────────┐
│ zfs-guid (root)              │
│ 白名单 / PAM / 审计           │
│ cap_enter() 后仍可用的 fd 池  │
│ kqueue task: progress/cancel │
└──────────────┬───────────────┘
               │ 预连接 Helper 池 (5)
┌──────────────▼───────────────┐
│ zfs-helper (root)            │
│ mount / send_stream /         │
│ receive_stream / unmount      │
│ fork+exec 独立进程组          │
└──────────────────────────────┘
```

### 1. kqueue 异步任务

`daemon/zg-task.c` 用 kqueue 同时监听 GUI 与 Helper：

- `EVFILT_READ` 处理双向帧
- `EVFILT_TIMER` 处理取消超时
- `zg-frame.c` 处理半包/粘包，单帧上限 1MB
- 所有 socket 用 `recv(MSG_DONTWAIT)`，EAGAIN 立即返回事件循环

流任务状态：

```
STARTED → PROGRESS → COMPLETED
              └──→ CANCELED / FAILED
```

### 2. SCM_RIGHTS 文件描述符

旧实现让 daemon 按路径打开备份文件；Capsicum 下不可行。

新实现：

1. GUI 自己打开文件
2. 通过 `sendmsg(SCM_RIGHTS)` 把只读/只写 fd 传给 daemon
3. daemon 再转交 Helper
4. Helper 不解析路径，只使用 fd

因此 `zfs send` 输出、`zfs receive` 输入都不需要 daemon 访问文件系统路径。

### 3. Capsicum

`zfs-guid` 在进入 `cap_enter()` 前：

- 预连接 5 个 Helper socket
- 对 fd 设置 `CAP_READ | CAP_WRITE | CAP_EVENT | CAP_GETSOCKOPT`
- 打开 `/dev/zfs` 并通过 libzfs fd 注入初始化

`CAP_EVENT` 是 kqueue 注册的必需权限；缺少它时注册会返回
`ENOTCAPABLE`，这是 v1.1.0 修复的关键问题之一。

### 4. 进度代理

- **send**：`zfs send` 写入 pipe，Helper 转发到 GUI fd，每 8MB/1s 报进度
- **receive**：源 fd 读入 pipe，Helper 用 poll 同时监听源、pipe 和取消 socket

### 5. 精确取消

- GUI 断开或点击取消
- daemon 向 Helper 发 CANCEL
- Helper 对 `zfs send/receive` 进程组发 `SIGTERM`
- 最终状态回到 GUI：`CANCELED`

---

## 安全模型

| 层 | 机制 |
| --- | --- |
| 操作白名单 | `common/zg-opdef.c`，未知操作直接拒绝 |
| 参数校验 | 位置参数、选项、数据集名、快照名逐项验证；不经过 shell |
| 危险操作 | `auth_mode = dangerous` 时走 PAM 二次认证，失败锁定 |
| 进程隔离 | daemon 进入 Capsicum；Helper 独立进程、独立进程组 |
| fd 传递 | SCM_RIGHTS，不依赖路径；daemon 不打开用户文件 |
| Helper 鉴权 | `getpeereid()` 要求 uid=0；socket 权限 0600 |
| 审计 | 每次操作写 uid/op/result/耗时；敏感操作记命令 |
| 严格模式 | `strict_mode = on` 时拒绝未迁移到安全路径的操作，不回退 `execv` |

GUI 自身**不写全局 GTK 设置、XSettings 或桌面主题**。界面只使用应用内样式。

---

## 安装

### 一键安装（推荐）

```sh
sh scripts/install-pkg.sh
```

脚本会：

1. 检查 `gtk4` / `glib` / `zfs` / `/dev/zfs` / PAM
2. 只安装缺失的运行时依赖
3. 安装 `dist/zfs-gui-1.1.0.pkg`
4. 清理 pid/socket
5. 启动并验证 `zfsguid`

### 手动安装

```sh
doas pkg install -y gtk4 glib
doas pkg install -y ./dist/zfs-gui-1.1.0.pkg
doas service zfsguid start
```

### 卸载

```sh
sh scripts/install-pkg.sh --uninstall
```

---

## 源码结构

```text
common/
  zg-opdef.c       白名单操作定义（唯一权限来源）
  zg-argv.c        参数解析与校验
  zg-json.c        轻量 JSON
daemon/
  zfs-guid.c       入口
  zg-server.c      AF_UNIX、鉴权、capsicum 启动
  zg-ops.c         操作分发
  zg-task.c        kqueue 流任务（send/receive）
  zg-frame.c       非阻塞长度前缀帧
  zg-helper.c      Helper 池客户端
  zg-progress.c    STARTED/PROGRESS 帧
  zg-policy.c      用户/组、strict_mode、capsicum_mode
helper/
  zfs-helper.c     特权 Helper：mount/send/receive/unmount
src/
  zg-client.c      GUI 客户端（fd 传递、流式帧）
  zg-page-*.c      存储池/数据集/快照/发送接收等页面
tests/
  scm_rights_test.c
  kqueue_frame_test.c
  frame_buffer_test.c
  kqueue_echo_test.c
  kqueue_task_test.c
  send_stream_chain.py
  receive_stream_chain.py
packaging/
  build-pkg.sh     FreeBSD pkg 打包
scripts/
  install-pkg.sh   一键安装
  selftest.sh      自检
  check-strict-c.sh
```

---

## 构建与测试

```sh
meson setup build --prefix=/usr/local
ninja -C build

sh scripts/check-strict-c.sh
sh scripts/selftest.sh
sh packaging/build-pkg.sh
```

测试覆盖：

- SCM_RIGHTS fd 传递、CLOEXEC、截断检测、100 次无泄漏
- kqueue 半包/粘包、双工 relay
- kqueue task mock：request+fd、started/progress/cancel/final
- send/receive 真实流与 `pgrep zfs send` 取消断言

---

## 文档

- [架构与安全（中文）](README.zh-CN.md)
- [Architecture & security（English）](README.en.md)
- [Capsicum 实验记录](docs/CAPSICUM.md)
- [特权 Helper](docs/HELPER.md)
- [kqueue 事件循环](docs/EVENT-LOOP.md)
- [libzfs fd 注入](docs/LIBZFS-FD-INJECTION.md)

---

## 下载

- GitHub Release：<https://github.com/Nickhades/zfs-gui/releases/tag/v1.1.0>
- 安装包：`zfs-gui-1.1.0.pkg`（Release 资产）
- 源码包：`zfs-gui-1.1.0-src.tar.zst`（Release 资产）
- 本地构建产物：`dist/zfs-gui-1.1.0.pkg`
- 版本：v1.1.0
- 许可：BSD-2-Clause
