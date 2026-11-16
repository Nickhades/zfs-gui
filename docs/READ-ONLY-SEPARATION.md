# 只读查询与 libzfs 分离进度

目标：GUI 的只读查询不再经过守护进程的 fork/exec，写操作仍由守护进程负责。

## 已迁移到 GUI 本地 libzfs

| 功能 | 入口 | 状态 |
|---|---|---|
| 数据集列表 + 10 秒刷新 | `zg_libzfs_datasets()` | 已迁移 |
| 存储池列表 + 10 秒刷新 | `zg_libzfs_pools()` | 已迁移 |
| 存储池 vdev 树/健康/错误/容量/扫描 | `zg_libzfs_pool_statuses()` | 已迁移（由 config/vdev_stats/scan_stats 合成） |
| 快照列表 | `zg_libzfs_snapshots()` | 已迁移，实测 26/26 与 `zfs list -t snapshot` 一致 |
| 数据集属性（zfs.get all） | `zg_libzfs_dataset_props()` | 本轮迁移：`zprop_iter` + `zfs_prop_get` 读取原生属性，`zfs_get_user_props` 读取用户属性；失败回退 CLI |

这几项覆盖了 GUI 中执行 `zfs list` / `zpool list` / `zpool status` 的主要列表场景。

## 仍走守护进程的只读查询

| 调用 | 页面 | 原因 / 计划 |
|---|---|---|
| `zfs.userspace` / `zfs.groupspace` | 数据集配额 | `zfs_userspace()` 有回调，但输出格式与分页需对齐 |
| `zfs.holds` | 快照页详情 | `zfs_get_holds()` 返回 nvlist，需要格式化层 |
| `zpool.status`（单池详情/状态页） | 存储池右栏 | 合成器已能提供树/状态，但“状态详情”页仍显示原命令文本，需要改写详情渲染 |
| `zpool.get` | 存储池属性页 | 属性列表/来源需映射 |
| `zpool.history` / `zpool.iostat` | 历史 / I/O 统计 | 数据量大，nvlist/文本格式需单独设计 |
| `bectl.list` | 启动环境 | libbe 与 base libzfs 的 ABI 冲突风险，暂缓 |
| `system.disks` | 添加/替换磁盘 | 目前由守护进程内部读取（无 exec），优先级低 |
| `system.info` / `system.arcstats` | 系统页 | 守护进程内部实现（sysctl，无 exec），不需要迁移 |

## 为什么“全部”不能一次完成

- `zfs.get all` 与 `zpool get all` 的输出包含属性来源、用户属性、不可设置属性，
  需要 libzfs nvlist 遍历 + 属性来源映射，属于独立的适配层
- `history`/`iostat` 是长时间序列，现有页面按文本列渲染，改成结构化数据需要 UI 同步调整
- 详情面板目前直接显示命令行输出，迁移后要改成“合成文本/结构化视图”

## 下一步顺序

1. `zpool.get` + 单池详情面板
3. `zfs.holds` / `zfs.userspace`
4. `history` / `iostat`
5. 最后处理 bectl 与 system.disks

每迁移一项都保留守护进程回退，直到该页验证通过后再删除旧路径。
