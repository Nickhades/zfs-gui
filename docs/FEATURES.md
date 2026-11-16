# 功能覆盖矩阵

本表列出注册表中的全部操作，以及它们在界面中的入口。
「命令中心」可以访问**所有**条目（按类别筛选 + 关键字搜索 + 自动表单）。

图例：🖥 = 专用界面（向导 / 工具栏按钮），🧭 = 命令中心通用表单，⚠ = 破坏性操作（二次确认）。
🔐 = 需要管理员密码（PAM 二次认证）。
---

## 1. 存储池生命周期

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 创建存储池 | `zpool create` | `zpool.create` ⚠ | 🖥 存储池 → 创建池（拓扑/设备/ashift/压缩/挂载点） |
| 销毁存储池 | `zpool destroy` | `zpool.destroy` ⚠ | 🖥 存储池 → 销毁；右键 → 销毁 |
| 导出存储池 | `zpool export` | `zpool.export` | 🖥 存储池 → 导出；右键 → 导出 |
| 导入存储池 | `zpool import` | `zpool.import` | 🖥 存储池 → 导入（`-d` 扫描目录、`-a`、`-N`、`-R`、`-f`、`-F`） |
| 升级池版本 | `zpool upgrade` | `zpool.upgrade` | 🧭 存储池 → 升级池版本（或 全部操作…） |
| 重新生成 GUID | `zpool reguid` | `zpool.reguid` ⚠ | 🧭 存储池 |
| 拆分镜像池 | `zpool split` | `zpool.split` ⚠ | 🧭 存储池 |

## 2. vdev 拓扑管理

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 添加 vdev | `zpool add` | `zpool.add` ⚠ | 🧭 存储池（可加 `mirror`/`raidz*`/`log`/`cache`/`spare`） |
| 移除 vdev | `zpool remove` | `zpool.remove` ⚠ | 🧭 存储池 |
| 附加镜像盘 | `zpool attach` | `zpool.attach` | 🧭 存储池（`-w` 等待、`-s` 顺序 resilver） |
| 拆除镜像盘 | `zpool detach` | `zpool.detach` ⚠ | 🧭 存储池 |
| 替换设备 | `zpool replace` | `zpool.replace` | 🧭 存储池 |
| 设备上线 | `zpool online` | `zpool.online` | 🧭 存储池 |
| 设备下线 | `zpool offline` | `zpool.offline` | 🧭 存储池 |
| 清除错误 | `zpool clear` | `zpool.clear` | 🖥 存储池 → 清除错误；右键 |
| 重新打开设备 | `zpool reopen` | `zpool.reopen` | 🧭 存储池（扩容后识别新容量） |
| 清除 ZFS 标签 | `zpool labelclear` | `zpool.labelclear` ⚠ | 🧭 存储池 |

## 3. 校验与维护

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 开始/停止/暂停 scrub | `zpool scrub` | `zpool.scrub` | 🖥 存储池 → scrub；右键（`-s`/`-p`/`-C`/`-a`） |
| TRIM | `zpool trim` | `zpool.trim` | 🖥 存储池 → TRIM；右键（`-d`/`-r` 限速） |
| 初始化空闲块 | `zpool initialize` | `zpool.initialize` | 🧭 存储池 |
| 重建冗余 | `zpool resilver` | `zpool.resilver` | 🧭 存储池 |
| 检查点 | `zpool checkpoint` | `zpool.checkpoint` ⚠ | 🧭 存储池（`-d` 丢弃） |
| 预取元数据 | `zpool prefetch` | `zpool.prefetch` | 🧭 存储池 |
| 同步数据 | `zpool sync` | `zpool.sync` | 🧭 存储池 |
| 等待后台活动 | `zpool wait` | `zpool.wait` | 🧭 存储池 |
| 裁剪去重表 | `zpool ddtprune` | `zpool.ddtprune` ⚠ | 🧭 存储池 |

## 4. 池信息查询

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 池列表 | `zpool list` | `zpool.list` | 🖥 存储池（刷新时自动） |
| 池状态 / vdev 树 | `zpool status` | `zpool.status` | 🖥 存储池左侧树 + 状态详情页 |
| I/O 统计 | `zpool iostat` | `zpool.iostat` | 🖥 存储池 → I/O 统计页（`-v`） |
| 读取属性 | `zpool get` | `zpool.get` | 🖥 存储池 → 属性页 |
| 修改属性 | `zpool set` | `zpool.set` | 🖥 属性页 → 编辑属性 |
| 操作历史 | `zpool history` | `zpool.history` | 🖥 存储池 → 历史页 |
| 事件流 | `zpool events` | `zpool.events` | 🧭 系统（`-v`/`-f`/`-c`） |

## 5. 数据集

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 创建数据集 | `zfs create` | `zfs.create` | 🖥 数据集 → 创建数据集（压缩/配额/预留/挂载点/附加属性） |
| 创建 zvol | `zfs create -V` | `zfs.create.volume` | 🖥 数据集 → 创建数据集 → 类型选「卷」（大小/块大小） |
| 销毁数据集/快照 | `zfs destroy` | `zfs.destroy` ⚠ | 🖥 数据集 → 销毁；快照页 → 销毁 |
| 重命名 | `zfs rename` | `zfs.rename` | 🖥 数据集 → 重命名 |
| 克隆快照 | `zfs clone` | `zfs.clone` | 🖥 快照 → 克隆 |
| 提升克隆 | `zfs promote` | `zfs.promote` ⚠ | 🧭 数据集（全部操作…） |
| 挂载 | `zfs mount` | `zfs.mount` | 🖥 数据集 → 挂载 |
| 卸载 | `zfs unmount` | `zfs.unmount` | 🖥 数据集 → 卸载 |
| 共享 NFS/SMB | `zfs share` | `zfs.share` | 🧭 数据集 |
| 取消共享 | `zfs unshare` | `zfs.unshare` | 🧭 数据集 |
| 升级数据集版本 | `zfs upgrade` | `zfs.upgrade` | 🧭 数据集 |
| 重写数据块 | `zfs rewrite` | `zfs.rewrite` | 🧭 数据集 |
| 等待活动 | `zfs wait` | `zfs.wait` | 🧭 数据集 |

## 6. 属性与配额

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 列出数据集 | `zfs list` | `zfs.list` | 🖥 数据集（刷新时自动） |
| 读取属性 | `zfs get` | `zfs.get` | 🖥 数据集 → 属性页（含来源） |
| 设置属性 | `zfs set` | `zfs.set` | 🖥 属性页 → 编辑属性 |
| 继承属性 | `zfs inherit` | `zfs.inherit` | 🖥 属性页 → 恢复继承 |
| 用户空间占用 | `zfs userspace` | `zfs.userspace` | 🖥 数据集 → 用户配额页 |
| 组空间占用 | `zfs groupspace` | `zfs.groupspace` | 🖥 数据集 → 组配额页 |
| 项目空间占用 | `zfs projectspace` | `zfs.projectspace` | 🧭 属性 |
| 项目配额 | `zfs project` | `zfs.project` | 🧭 属性 |

## 7. 快照与书签

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 创建快照 | `zfs snapshot` | `zfs.snapshot` | 🖥 快照 → 创建快照（递归 / 自动命名）；数据集 → 快照 |
| 回滚 | `zfs rollback` | `zfs.rollback` ⚠ | 🖥 快照 → 回滚；右键 |
| 克隆 | `zfs clone` | `zfs.clone` | 🖥 快照 → 克隆 |
| 创建书签 | `zfs bookmark` | `zfs.bookmark` | 🖥 快照 → 书签 |
| 脱敏书签 | `zfs redact` | `zfs.redact` | 🧭 快照 |
| 比较差异 | `zfs diff` | `zfs.diff` | 🖥 快照 → 差异 |
| 添加 hold | `zfs hold` | `zfs.hold` | 🖥 快照 → hold |
| 查看 hold | `zfs holds` | `zfs.holds` | 🖥 快照页右侧自动加载 |
| 解除 hold | `zfs release` | `zfs.release` | 🖥 快照 → release |

## 8. 发送 / 接收

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 发送快照 | `zfs send` | `zfs.send` | 🖥 发送/接收页；快照 → 发送（全量/`-i`/`-R`/`-I`，`-c`、`-n`） |
| 接收快照流 | `zfs receive` | `zfs.receive` | 🖥 发送/接收页（`-F`、`-u`、`-n`，`-o`/`-x`） |

## 9. 加密

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 加载密钥 | `zfs load-key` | `zfs.load-key` | 🖥 数据集 → 加密/密钥 |
| 卸载密钥 | `zfs unload-key` | `zfs.unload-key` | 🖥 数据集 → 加密/密钥 |
| 更换密钥 | `zfs change-key` | `zfs.change-key` ⚠ | 🖥 数据集 → 加密/密钥 |

## 10. 委派权限

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 授予委派权限 | `zfs allow` | `zfs.allow` | 🖥 数据集 → 委派 |
| 撤销委派权限 | `zfs unallow` | `zfs.unallow` | 🖥 数据集 → 委派 |

## 11. FreeBSD 特有

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 数据集挂接到 jail | `zfs jail` | `zfs.jail` | 🧭 数据集 |
| 解除 jail 绑定 | `zfs unjail` | `zfs.unjail` | 🧭 数据集 |
| 列出启动环境 | `bectl list` | `bectl.list` | 🖥 启动环境页 |
| 创建启动环境 | `bectl create` | `bectl.create` | 🖥 启动环境 → 创建启动环境 |
| 激活启动环境 | `bectl activate` | `bectl.activate` | 🖥 启动环境 → 激活 |
| 重命名启动环境 | `bectl rename` | `bectl.rename` | 🖥 启动环境 → 重命名 |
| 挂载启动环境 | `bectl mount` | `bectl.mount` | 🖥 启动环境 → 挂载 |
| 卸载启动环境 | `bectl unmount` | `bectl.unmount` | 🖥 启动环境 → 卸载 |
| 以 jail 运行 BE | `bectl jail` | `bectl.jail` | 🧭 启动环境 |
| 结束 BE jail | `bectl unjail` | `bectl.unjail` | 🧭 启动环境 |
| 导出启动环境 | `bectl export` | `bectl.export` | 🖥 启动环境 → 导出 |
| 导入启动环境 | `bectl import` | `bectl.import` | 🖥 启动环境 → 导入 |
| 销毁启动环境 | `bectl destroy` | `bectl.destroy` ⚠ | 🖥 启动环境 → 销毁 |

## 12. 系统信息

| 功能 | 命令 | 操作 ID | 界面入口 |
| --- | --- | --- | --- |
| 能力探测 | 内部实现 | `system.info` | 🖥 启动时自动；系统页「刷新系统信息」 |
| ARC 统计 | `sysctl kstat.zfs.misc.arcstats` | `system.arcstats` | 🖥 系统页（命中率摘要 + 全量表格） |
| jail 列表 | `jls -h jid name path` | `system.jails` | 🧭 系统 |
| ZFS 版本 | `zfs version` | `zfs.version` | 🖥 系统页 / 标题栏副标题 |

---

## 13. 危险操作与二次认证

下列 27 个操作在守护进程侧强制要求管理员密码（`zg_op_requires_password()`），
前端会先弹出「需要管理员授权」窗口：

| 领域 | 操作 ID |
| --- | --- |
| 存储池 | `zpool.create`、`zpool.destroy`、`zpool.export`、`zpool.import`、`zpool.upgrade`、`zpool.reguid`、`zpool.split` |
| vdev | `zpool.add`、`zpool.remove`、`zpool.attach`、`zpool.detach`、`zpool.replace`、`zpool.labelclear` |
| 维护 | `zpool.checkpoint`、`zpool.ddtprune` |
| 数据集 | `zfs.destroy`、`zfs.rollback`、`zfs.promote`、`zfs.rewrite`、`zfs.upgrade` |
| 快照 | `zfs.redact` |
| 加密 | `zfs.change-key` |
| 委派 | `zfs.allow`、`zfs.unallow` |
| 发送接收 | `zfs.receive` |
| 启动环境 | `bectl.destroy`、`bectl.jail` |

其余 39 个写操作只需界面确认（显示完整命令），20 个只读操作直接执行。
把 `auth_mode` 设为 `all` 后，确认级别的操作也会要求密码。

---

## 14. 未开放的功能及原因

| 功能 | 原因 | 替代方式 |
| --- | --- | --- |
| `zfs program`（Lua 脚本） | 等价于在内核执行任意脚本，违背白名单命令模型 | 在终端中执行 |
| `zfs send \| zfs receive` 管道直连 | 需要复合管道操作，当前以文件中转 | 两个操作分别执行 |
| `--json` 之外的实验性参数 | 版本差异大，不做绑定 | 命令中心不暴露非标准开关 |

---

## 15. 常用「一键」路径速查

| 目标 | 路径 |
| --- | --- |
| 新建一个冗余池 | 存储池 → 创建池 → 镜像/RAID-Z → 填设备 |
| 给池加盘扩容 | 存储池 → 全部操作… → 添加 vdev |
| 换一块坏盘 | 存储池 → 全部操作… → 替换设备（`zpool.replace`） |
| 定期快照 | 快照 → 创建快照 → 勾选递归 |
| 误删文件回滚 | 快照 → 选中快照 → 回滚 |
| 把快照备份到移动盘 | 快照 → 发送 → 输出到 `/mnt/usb/xxx.zfs` |
| 在另一台机恢复 | 发送/接收 → 接收 → 输入文件 + 目标数据集 |
| 升级前后回滚系统 | 启动环境 → 创建启动环境；出问题 → 激活旧 BE 重启 |
| 查 scrub 进度 | 存储池 → 状态详情（`scan:` 行） |
| 查谁占了空间 | 数据集 → 用户配额 / 组配额 |
| 给普通用户有限权限 | 数据集 → 委派 → `zfs allow -u user …` |
