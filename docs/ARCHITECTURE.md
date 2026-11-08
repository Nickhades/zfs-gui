# 架构设计

本文说明 ZFS-GUI 的进程模型、通信协议、安全校验与前端结构，以及为兼容不同
FreeBSD / OpenZFS 版本所做的取舍。

---

## 1. 进程与线程模型

```
          ┌──────────────────────────────┐
          │ zfs-gui （GTK4 主线程）        │
          │  · 构建界面                    │
          │  · GTask 工作线程发起 IPC       │
          └──────────────┬───────────────┘
                         │ connect /send /recv
                         ▼
          ┌──────────────────────────────┐
          │ zfs-guid 主进程（root）        │
          │  poll(监听 socket, 自管道)      │
          │  accept → getpeereid → fork   │
          └──────────────┬───────────────┘
                         │ fork per connection（≤ 64 并发）
                         ▼
          ┌──────────────────────────────┐
          │ 连接处理子进程                  │
          │  读帧 → 校验 → 执行 → 回帧      │
          └──────────────┬───────────────┘
                         │ fork + execv + 独立进程组
                         ▼
          ┌──────────────────────────────┐
          │ zpool / zfs / bectl / sysctl  │
          └──────────────────────────────┘
```

* **每个连接一个子进程**：`zpool scrub -w`、`zfs receive` 这类长任务不会阻塞其它客户端。
* 主进程用「自管道 + `poll()`」处理 `SIGCHLD`/`SIGTERM`，避免 `accept()` 中被信号打断的竞态；
  自管道读端设为非阻塞，排空时不会卡死。
* 子进程把 `SIGCHLD`/`SIGTERM` 恢复为默认，并使用独立进程组，便于超时后整组终止。
* 子进程关闭监听 socket 与自管道；所有外部 fd 置 `FD_CLOEXEC`，避免泄漏给 `zfs`。

---

## 2. 通信协议

### 2.1 帧

```
+--------+--------------------------------+
| u32 BE |  UTF-8 JSON 载荷（N 字节）      |
+--------+--------------------------------+
```

* 4 字节大端长度 + 载荷，读写循环处理 `EINTR` 与短写。
* 单帧上限 `ZG_MAX_FRAME`（32 MiB），防止恶意长度导致内存耗尽。
* 接受请求超时 60 s；应答不设读超时（长任务）；发送超时 60 s。

### 2.2 请求

```json
{
  "v": 1,
  "op": "zpool.destroy",
  "args": { "positional": ["tank"] },
  "auth": { "user": "root", "password": "……" }
}
```

* `auth` 只对危险操作需要；守护进程校验通过后立即使用并清零，不落盘、不进日志。
* 只读操作携带 `auth` 会被忽略。

* `flags`：布尔开关，必须逐字出现在该操作的允许列表中。
* `options`：带值选项，统一按 `-<key> <value>` 展开。
* `positional`：位置参数，按顺序追加到 argv 末尾。
* `output` / `input`：仅 `zfs send` / `zfs receive` 使用，用于文件重定向。

### 2.3 应答

```json
{
  "v": 1,
  "ok": true,
  "code": 0,
  "stdout": "…",
  "stderr": "",
  "message": "命令执行成功",
  "data": { }
}
```

* `code` 为子进程退出码；策略层拒绝时为负数。
* `data` 仅内部操作（`system.info`）返回结构化对象。
* 前端对 `stdout`/`stderr`/`message` 统一做 UTF-8 清洗，避免任意字节流入 Pango。

---

## 3. 操作注册表（白名单）

`common/zg-opdef.c` 中的静态表是**唯一可信的操作定义**，同时服务三方：

| 使用方 | 用途 |
| --- | --- |
| 守护进程 | 授权：操作是否存在、开关/选项/参数是否合法 |
| 前端命令中心 | 生成表单：标题、说明、参数类型、开关与选项 |
| argv 组装 | 工具名、子命令、参数顺序 |

条目结构：

```c
typedef struct {
  const char *id;            /* "zpool.create" */
  const char *tool;          /* "zpool" / "zfs" / "bectl" / "jls" / NULL */
  const char *subcommand;    /* 可多段，如 "import" */
  const char *title;         /* 中文标题 */
  const char *description;   /* 中文说明 */
  ZgOpCategory category;     /* 存储池/数据集/快照/… */
  gboolean destructive;      /* 需要二次确认 */
  gboolean long_running;     /* 不设超时 */
  ZgOpIo io;                 /* 是否重定向到文件 */
  const char *io_arg;
  ZgOpSpec spec;             /* flags/options/positional 与类型 */
} ZgOpDef;
```

### 3.1 校验规则

守护进程按以下顺序校验，任一失败立即返回：

1. 协议版本必须等于 `ZG_PROTOCOL_VERSION`。
2. `op` 必须能在注册表中查到。
3. 每个 `flag` 必须以 `-` 开头，且在该操作允许列表中。
4. 每个 option 的 key 必须在该操作允许列表中；value 非空、不以 `-` 开头、无换行；
   `-o`/`-O` 的值必须是 `property=value` 或合法的字段名列表；`-x` 必须是合法属性名。
5. 位置参数数量必须落在 `[min_positional, max_positional]` 内。
6. 每个位置参数按声明类型校验字符集（见下）。
7. 需要重定向的操作必须给出绝对路径的 `output`/`input`。

### 3.2 位置参数类型

| 类型 | 规则 |
| --- | --- |
| `ZG_VALUE_POOL` | 字母数字与 `_.:-`，不含 `/` |
| `ZG_VALUE_DATASET` | 字母数字与 `_.:-/`（可含 `@`、`#`、`%`），无空格 |
| `ZG_VALUE_SNAPSHOT` | 数据集名且必须含 `@` 或 `#` |
| `ZG_VALUE_VDEV` | `/dev/...` 路径，或 `mirror`/`raidz*`/`draid*`/`log`/`cache`/`spare` 等关键字 |
| `ZG_VALUE_PATH` | 非空、不以 `-` 开头 |
| `ZG_VALUE_NAME` / `TAG` | 简单名称 |
| `ZG_VALUE_USER` / `GROUP` | 用户名/组名或数字 |
| `ZG_VALUE_NUMBER` | 纯十进制 |
| `ZG_VALUE_PROPERTY` | `property=value` 或裸属性名 |
| `ZG_VALUE_FREE` | 非空、不以 `-` 开头、无控制字符 |

> 关键不变量：**所有位置参数与选项值都不得以 `-` 开头**。
> 配合「不经过 shell + execv 直接传 argv」，即使 ZFS 参数语义变化也不会产生注入。

### 3.3 argv 组装

```
argv = [工具绝对路径] + 子命令分词 + flags + (-key value)... + positional
```

工具路径由 `zg_exec_resolve_tool()` 依次尝试
`/sbin`、`/usr/sbin`、`/usr/local/sbin`、`/bin`、`/usr/bin`、`/usr/local/bin`，
最后回退 `PATH`，因此同时兼容 base 系统 ZFS 与 `sysutils/openzfs`。

---

## 4. 执行层

`daemon/zg-exec.c` 的 `zg_exec_run()`：

* `fork()` 后子进程 `setpgid(0,0)`，`execve()` 固定环境：
  `PATH`、`LC_ALL=C`、`LANG=C`、`TZ=UTC`；
* 父进程用 `poll()` 同时排空 stdout/stderr，累计上限 `ZG_MAX_OUTPUT`（64 MiB），
  超限后继续读取并丢弃，避免子进程因管道写满而死锁；
* 超时（`io_timeout`，长任务为 0 即不限）后先 `SIGTERM` 整个进程组，
  2 s 内未退出则 `SIGKILL`；
* stdin 默认接 `/dev/null`，`zfs receive` 时接输入文件；
* `stdout` 需要落盘时（`zfs send`）直接 `dup2` 到以 `0600` 创建的文件，
  避免把大数据流读入内存。

---

## 5. 鉴权与配置

鉴权分两层：

**第一层：连接身份**（谁能连上守护进程）

```
accept() → getpeereid() → uid==0 放行
        → getpwuid() 命中 allowed_users 放行
        → getgrouplist() 命中 allowed_groups 放行
        → 否则拒绝并记录日志
```

**第二层：操作身份**（谁批准了这次危险操作）

注册表为每个操作标注 `ZgAuthLevel`：

| 等级 | 含义 | 守护进程行为 |
| --- | --- | --- |
| `ZG_AUTH_READ` | 只读 | 直接执行 |
| `ZG_AUTH_CONFIRM` | 修改状态 | 直接执行（前端负责确认） |
| `ZG_AUTH_PASSWORD` | 危险 | 必须携带 `auth`，PAM 验证通过后才执行 |

`auth_mode` 决定上述阈值：

* `dangerous`（默认）：只有 `ZG_AUTH_PASSWORD` 需要密码；
* `all`：`ZG_AUTH_CONFIRM` 及以上都需要密码；
* `none`：从不要求（仅用于隔离测试环境，界面会显示红色不安全徽章）。

PAM 验证流程（`daemon/zg-auth.c`）：

1. 先按 uid 查失败计数（`<socket 目录>/.authfail-<uid>`，含 `flock` 保护的计数与时间戳）；
   超过 `auth_max_attempts` 且未过 `auth_lockout` 秒 → 直接返回 `ZG_ERR_AUTH_LOCKED`。
2. `pam_start("zfs-gui", user, conv)` → `pam_authenticate` → `pam_acct_mgmt`；
   若系统没有 `zfs-gui` 服务配置，回退到 `system`。
3. 认证通过后还要确认该账号属于管理员集合（root / allowed_users / allowed_groups），
   否则返回 `ZG_ERR_AUTH_FAILED` —— 防止用普通账号的密码绕过。
4. 成功清除失败计数；失败累加并写 syslog。

密码只以可变副本存在于 `zg_auth_verify()` 栈上，函数返回前 `explicit_bzero()` 清零。

前端流程（`src/zg-dialogs.c`）：

```
zg_dialogs_run_op()
  ├─ 需要密码 → zg_dialogs_password() ── 用户提交 ──▶ 带 auth 调用
  ├─ 破坏性   → zg_dialogs_confirm() ── 确认 ─────▶ 不带 auth 调用
  └─ 只读     → 直接调用
收到 ZG_ERR_AUTH_REQUIRED / FAILED 时重新弹出授权窗口（最多 3 次），
锁定（ZG_ERR_AUTH_LOCKED）则直接报错，不再重试。
```

```
/usr/local/etc/zfs-gui.conf
  allowed_users     = root
  allowed_groups    = wheel
  socket_path       = /var/run/zfs-gui/zfs-gui.sock
  socket_group      = wheel
  socket_mode       = 0660
  io_timeout        = 600
  auth_mode         = dangerous
  auth_max_attempts = 3
  auth_lockout      = 300
```

socket 文件权限作为物理防线（默认只有 `wheel` 可连接），守护进程内的两道判定
（连接身份 + 操作身份）作为逻辑防线。

---

## 6. 前端结构

### 6.1 应用上下文 `ZgApp`

* 持有 `GtkApplication`、主窗口、状态栏、连接提示条；
* 保存 `system.info` 返回的能力信息（ZFS 版本、是否支持 `-j`、socket 路径）；
* 维护刷新钩子列表：每个页面注册自己的 `refresh(data)`，点击「刷新」或任何操作完成后
  由 `zg_app_refresh_all()` 统一触发；
* 维护任务日志（环形截断到 512 KiB），系统页实时显示；
* 提供 Toast 提示（右下角浮层，3.2 秒自动隐藏）与安全状态徽章；
* 保存 `auth_mode`，供 UI 决定是否提前弹出管理员授权窗口。

### 6.2 异步调用

```c
zg_client_call_async (op, args_json, callback, user_data);
ZgClientResult *result = zg_client_call_finish (async_result);
```

内部使用 `GTask` + `g_task_run_in_thread()`，连接、发包、收包、JSON 解析都在工作线程，
回调回到主线程。因此界面不会因为 `zfs send` 之类的长命令冻结。

### 6.3 页面模式

每个页面（`src/zg-page-*.c`）都是：

1. 构建左侧列表/树 + 右侧详情页签；
2. 通过 `zg_app_register_refresh()` 注册 `xxx_refresh()`；
3. 所有 ZFS 调用都是异步，回调里更新对应控件；
4. 选择变化时加载详情（多路并发请求，用 `selected` 名称做陈旧结果丢弃）；
5. 工具栏按钮 → 专用向导或 `zg_dialogs_run_op()`/`zg_dialogs_generic_op()`。

### 6.4 对话框

`src/zg-dialogs.c` 提供：

* `zg_dialogs_confirm()`：破坏性操作确认，显示完整命令行；
* `zg_dialogs_run_op()`：统一的「执行 + 结果展示 + 自动刷新」；
  破坏性操作自动插入确认步骤；只读操作的输出直接以窗口展示；
* `zg_dialogs_generic_op()`：按注册表自动生成表单（位置参数带类型提示、
  开关带中文说明、选项带值输入、实时命令预览）；
* 创建池/数据集/快照、销毁、属性编辑、发送快照等专用向导。

### 6.5 对象操作面板

`src/zg-actions.[ch]` 提供统一样式的操作面板：

```
┌────────────────────────────────────────────────────────────┐
│ [图标] 对象名                                    [状态胶囊] │
│        副标题（容量 / 挂载点 / 所属池）                      │
│ [属性] [创建快照] [卸载] [重命名] [配额] …   （动作按钮）    │
│ 提示：Enter 属性 · F2 重命名 · Delete 销毁 · F5 刷新        │
└────────────────────────────────────────────────────────────┘
```

每个页面在选中对象变化时调用 `zg_action_panel_set()` 重建按钮，
按钮的 `needs_password` 字段会在文字后追加锁图标，`danger` 使用
`destructive-action` 样式。

操作一律走**语义化对话框**（`src/zg-dialogs-objects.c` 等）：

| 对话框 | 用户看到的内容 | 生成的操作 |
| --- | --- | --- |
| `zg_dialogs_choice` | 标题 + 说明 + 勾选项 | 任意操作的选项式确认 |
| `zg_dialogs_input` | 单个输入框 | 重命名、克隆名、hold 标签 |
| `zg_dialogs_add_vdev` | 磁盘复选列表 + 用途 + 布局 | `zpool add` |
| `zg_dialogs_replace_disk` | 磁盘单选列表 | `zpool replace` |
| `zg_dialogs_import_pool` | 可导入池列表 + 扫描目录 | `zpool import` |
| `zg_dialogs_encryption` | 密钥状态 + 位置 | load/unload/change-key |
| `zg_dialogs_delegation` | 用户/组 + 权限复选 | `zfs allow` / `unallow` |
| `zg_dialogs_quota` | 用户/组 + 配额值 | `zfs set userquota@…` |
| `zg_dialogs_share` | NFS / SMB 开关 | `zfs set sharenfs/sharesmb` |

命令行等价物仍保留在「菜单 → 高级命令」中，复用同一套 `zg_dialogs_run_op()`，
因此无论从哪个入口执行，权限校验与审计路径完全一致。

### 6.6 表格组件

`src/zg-table.[ch]` 用 `GtkColumnView` 实现统一表格：

* 每行是真实控件，天然支持**行间横线**、斑马纹、悬停与选中样式；
* `GtkTreeListModel` 提供层级（存储池 → vdev → 磁盘、数据集树），并自动展开；
* `GtkSingleSelection` 单选，选中项通过 `zg_table_get_selected()` 取得；
* 行对象 `ZgRow` 最多 8 个文本列，并可用 `zg_row_set_tag()` 携带页面自己的业务数据；
* 列定义 `ZgTableColumn` 描述标题、列号、是否伸展、对齐与字重。

```c
GListStore *store = g_list_store_new (ZG_TYPE_ROW);
GtkWidget  *table = zg_table_new_tree (store, columns, G_N_ELEMENTS (columns));
```

刷新时只清空并重建 store，表格与选择模型保持不变。

### 6.7 主题与字体

* `main()` 在 GTK 初始化前设置 `GTK_THEME=Adwaita`（用户显式设置时不覆盖），
  `apply_desktop_theme()` 再确保 `gtk-application-prefer-dark-theme` 为假，
  因此界面始终是浅色。
* 调色板在 `src/zg-ui.c` 顶部用 `@define-color` 定义（`zg_bg`、`zg_surface`、
  `zg_accent` …），所有规则只引用这些变量；两段 CSS provider 各自带一份定义，
  避免跨 provider 解析不到颜色。
* 强调色为绿色（`#3fb883`），用于主按钮、页签下划线、进度条、侧栏选中项与
  表格选中行；危险操作为浅红底红字。
* 全局字体为 `Noto Sans CJK SC` 600 字重，标题 800 字重，
  通过在 CSS 中对 `window` 设置 `font-family` / `font-weight` 实现。

### 6.8 数据解析

`src/zg-model.c` 只做纯函数解析，便于单独测试：

| 输入 | 用途 |
| --- | --- |
| `zpool list -H -p` | 池容量/健康/碎片/去重 |
| `zpool status -j` 或文本 | vdev 树、状态、错误计数 |
| `zfs list -H -p -o name,used,avail,refer,mountpoint,type,creation,mounted` | 数据集树 |
| `zfs list -H -p -t snapshot -o name,used,refer,creation` | 快照列表 |
| `zfs get` / `zpool get -H -p` | 属性表（容量类自动格式化，`creation` 转本地时间） |
| `sysctl kstat.zfs.misc.arcstats` | ARC 统计与命中率 |

---

## 7. 兼容性取舍

| 需求 | 方案 | 原因 |
| --- | --- | --- |
| 精确数值 | `-p` | 避免人类可读值的舍入，便于排序与换算 |
| 稳定格式 | `-H` | 制表符分隔，与 locale/终端宽度无关 |
| vdev 层级 | `-j` 优先，文本回退 | `zpool status` 没有 `-H`；2.2+ 才有 `-j` |
| 时间 | `-p` 的 epoch，前端本地化 | 不依赖 ZFS 的日期格式 |
| 属性名 | 不硬编码白名单，交由 ZFS 校验 | 新版本属性自动可用 |

---

## 8. 已知边界

* **`zfs program`（Lua 脚本）未开放**：它等价于在内核中执行任意脚本，
  与「白名单命令」的安全模型冲突，故刻意不登记。
* **send | receive 管道直连未实现**：当前以文件为中介；
  需要管道时可组合使用「命令中心」两次操作，或后续增加复合操作。
* **不支持同时多实例写操作**：守护进程不串行化命令，同时发起互斥操作时由 ZFS 自行报错。
* **`zpool status -j` 不含 scan/errors 文本**：界面在选中池时额外拉取
  `zpool status -v` 以显示扫描进度与错误详情。

---

## 9. 测试

后端可脱离图形界面测试。仓库开发时使用的脚本思路：

```sh
# 1) 用非 root 方式启动测试实例
ZFS_GUI_ALLOW_NONROOT=1 ./build/zfs-guid -f -c /tmp/zg-test/zfs-gui.conf

# 2) 用 Python 构造帧请求，验证：
#    - system.info 能力探测
#    - zpool list / status -j / zfs list / get / arcstats / bectl list
#    - 未知操作、未授权开关、以 '-' 开头的参数、非法选项键、协议版本不符 全部被拒绝
```

前端验证：`ZFS_GUI_SOCKET=/tmp/zg-test/zfs-gui.sock ./build/zfs-gui`，
逐页检查数据加载与操作流程。
