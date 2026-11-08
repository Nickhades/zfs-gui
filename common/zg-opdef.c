/* zg-opdef.c - 后端命令注册表
 *
 * 覆盖 OpenZFS 2.x on FreeBSD 的 zpool / zfs / bectl 命令面。
 * 每个条目同时承担三个职责：
 *   - 守护进程的授权白名单（操作 ID、开关、选项、位置参数类型）
 *   - 前端「高级操作」对话框的表单元数据（标题、说明、参数提示）
 *   - 参数拼装规则（见 zg-argv.c）
 *
 * 注意：这里刻意不登记任何依赖新版本特性的开关之外的行为差异；
 * 某开关在旧系统上不受支持时，由 ZFS 自身报错并在界面显示。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-opdef.h"

#include <string.h>

/* 常用开关集合，减少重复书写 */
#define F(...)            \
    (const char *const[]) \
    {                     \
        __VA_ARGS__, NULL \
    }

static const ZgOpDef zg_operations[] = {
        /* ====================================================================== */
        /* 存储池生命周期                                                          */
        /* ====================================================================== */
        {
                .id = "zpool.create",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "create",
                .title = "创建存储池",
                .description = "按 vdev 拓扑新建存储池；支持 -f 强制、-n 演练、-o 池属性、"
                               "-O 文件系统属性、-m 挂载点、-R altroot。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-f", "-n", "-d"),
                        .options = (const char *const[]){"o", "O", "m", "R", NULL},
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.destroy",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "destroy",
                .title = "销毁存储池",
                .description = "永久销毁存储池及其全部数据集。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-f"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.export",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "export",
                .title = "导出存储池",
                .description = "从系统中导出池，供其他系统导入；-a 导出全部。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-a", "-f"),
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.import",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "import",
                .title = "导入存储池",
                .description = "扫描设备并导入池；-d 指定搜索目录，-a 导入全部，-N 不挂载，"
                               "-R altroot，-f 强制，-F 回滚事务。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-a", "-D", "-l", "-f", "-m", "-N", "-F", "-n"),
                        .options = (const char *const[]){"d", "o", "c", "R", NULL},
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.upgrade",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "upgrade",
                .title = "升级池版本",
                .description = "升级池的 on-disk 版本；-a 处理全部池，-V 指定版本。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-a"),
                        .options = (const char *const[]){"V", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.reguid",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "reguid",
                .title = "重新生成池 GUID",
                .description = "生成新的池 GUID，常用于克隆设备后避免重复。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .options = (const char *const[]){"g", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.split",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "split",
                .title = "拆分镜像池",
                .description = "把镜像池拆分为两个池；-n 演练，-R altroot。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-g", "-L", "-n", "-P", "-l"),
                        .options = (const char *const[]){"R", "o", NULL},
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },

        /* ====================================================================== */
        /* 池内设备拓扑                                                            */
        /* ====================================================================== */
        {
                .id = "zpool.add",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "add",
                .title = "添加 vdev",
                .description = "向池中添加新的 vdev（mirror/raidz/log/cache/spare 等）。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-a", "-f", "-g", "-L", "-n", "-P"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.remove",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "remove",
                .title = "移除 vdev",
                .description = "从池中移除设备或 top-level vdev；-n 演练，-w 等待完成。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-n", "-p", "-s", "-w"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.attach",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "attach",
                .title = "附加镜像设备",
                .description = "为单盘 vdev 附加镜像盘；-f 强制，-s 顺序 resilver，-w 等待。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-f", "-s", "-w"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 3,
                        .max_positional = 3,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.detach",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "detach",
                .title = "拆除镜像设备",
                .description = "从镜像 vdev 中拆除一块设备。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.replace",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "replace",
                .title = "替换设备",
                .description = "用新设备替换故障盘；-s 顺序 resilver，-w 等待完成。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-f", "-s", "-w"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 2,
                        .max_positional = 3,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.online",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "online",
                .title = "上线设备",
                .description = "把设备标记为在线；-e 扩展池容量。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-e"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.offline",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "offline",
                .title = "下线设备",
                .description = "把设备标记为离线以便安全拔盘；-t 临时，-f 强制。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-f", "-t"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.clear",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "clear",
                .title = "清除设备错误",
                .description = "清除池或设备的错误计数；-n 仅显示，-F 同时丢弃事务。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-n", "-F"),
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.reopen",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "reopen",
                .title = "重新打开设备",
                .description = "让池重新打开所有 vdev（用于设备扩容后识别新容量）。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-n"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.labelclear",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "labelclear",
                .title = "清除设备 ZFS 标签",
                .description = "擦除设备上的 ZFS 标签，使设备可被重新使用。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-f"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_VDEV},
                },
        },

        /* ====================================================================== */
        /* 池维护                                                                  */
        /* ====================================================================== */
        {
                .id = "zpool.scrub",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "scrub",
                .title = "开始/停止 scrub",
                .description = "校验并修复池中全部数据；-s 停止，-p 暂停，-w 等待完成，-a 全部池。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-a", "-e", "-s", "-p", "-C", "-w"),
                        .options = (const char *const[]){"E", "S", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.trim",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "trim",
                .title = "TRIM 释放空间",
                .description = "向底层设备发送 TRIM；-d 持续，-c/-s 取消/停止，-r 限速，-w 等待。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-a", "-d", "-w", "-c", "-s"),
                        .options = (const char *const[]){"r", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.initialize",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "initialize",
                .title = "初始化空闲块",
                .description = "写入空闲块以消除历史数据；-c 取消，-s 暂停，-u 恢复，-w 等待。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-a", "-c", "-s", "-u", "-w"),
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_VDEV},
                },
        },
        {
                .id = "zpool.resilver",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "resilver",
                .title = "重建冗余数据",
                .description = "对池中全部冗余 vdev 执行 resilver。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.checkpoint",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "checkpoint",
                .title = "创建/丢弃检查点",
                .description = "创建池检查点；-d 丢弃检查点（不可逆），-w 等待。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-d", "-w"),
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.prefetch",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "prefetch",
                .title = "预取元数据",
                .description = "把池元数据预取进 ARC；-t 指定类型。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .options = (const char *const[]){"t", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.sync",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "sync",
                .title = "同步池数据",
                .description = "把池中所有未写数据刷到磁盘。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.wait",
                .tool = "zpool",
                .subcommand = "wait",
                .title = "等待池活动",
                .description = "等待池的初始化/trim/替换等后台活动结束。",
                .category = ZG_CAT_POOL,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-H", "-p"),
                        .options = (const char *const[]){"T", "t", NULL},
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_NUMBER},
                },
        },
        {
                .id = "zpool.ddtprune",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zpool",
                .subcommand = "ddtprune",
                .title = "裁剪去重表",
                .description = "按比例裁剪 DDT；-d 数据，-p 百分比阈值。",
                .category = ZG_CAT_POOL,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-d", "-p"),
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_NUMBER, ZG_VALUE_POOL},
                },
        },

        /* ====================================================================== */
        /* 池信息查询                                                              */
        /* ====================================================================== */
        {
                .id = "zpool.list",
                .tool = "zpool",
                .subcommand = "list",
                .title = "列出存储池",
                .description = "以脚本或 JSON 方式列出池；-p 输出精确字节，-j 输出 JSON。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-g", "-H", "-L", "-p", "-P", "-v", "-j"),
                        .options = (const char *const[]){"o", "T", NULL},
                        .min_positional = 0,
                        .max_positional = 3,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_NUMBER, ZG_VALUE_NUMBER},
                },
        },
        {
                .id = "zpool.status",
                .tool = "zpool",
                .subcommand = "status",
                .title = "池状态",
                .description = "显示池与 vdev 健康状态；-v 显示错误详情，-j 输出 JSON，-x 仅异常。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-D", "-d", "-e", "-g", "-i", "-L", "-P", "-p", "-s", "-t", "-v", "-x", "-j"),
                        .options = (const char *const[]){"c", "T", NULL},
                        .min_positional = 0,
                        .max_positional = 3,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_NUMBER, ZG_VALUE_NUMBER},
                },
        },
        {
                .id = "zpool.iostat",
                .tool = "zpool",
                .subcommand = "iostat",
                .title = "I/O 统计",
                .description = "显示池与设备的读写统计；-y 跳过首行，-v 分 vdev 统计。",
                .category = ZG_CAT_POOL,
                .spec = {
                        .flags = F("-g", "-h", "-H", "-L", "-p", "-P", "-v", "-y", "-r", "-w", "-n", "-l", "-q"),
                        .options = (const char *const[]){"c", "T", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL, ZG_VALUE_NUMBER, ZG_VALUE_NUMBER},
                },
        },
        {
                .id = "zpool.get",
                .tool = "zpool",
                .subcommand = "get",
                .title = "读取池属性",
                .description = "读取池属性；-H 脚本输出，-p 精确数值，-j JSON，-o 选择字段。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-H", "-p", "-j"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.set",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zpool",
                .subcommand = "set",
                .title = "修改池属性",
                .description = "设置池属性，如 ashift、autoexpand、autotrim、comment。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_PROPERTY, ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.history",
                .tool = "zpool",
                .subcommand = "history",
                .title = "池操作历史",
                .description = "显示池的命令历史；-i 含内部事件，-l 长格式。",
                .category = ZG_CAT_SYSTEM,
                .spec = {
                        .flags = F("-i", "-l"),
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_POOL},
                },
        },
        {
                .id = "zpool.events",
                .tool = "zpool",
                .subcommand = "events",
                .title = "ZFS 事件流",
                .description = "显示内核 ZFS 事件；-v 详细，-f 持续跟随，-c 清空。",
                .category = ZG_CAT_SYSTEM,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-v", "-H", "-f", "-c"),
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_POOL},
                },
        },

        /* ====================================================================== */
        /* 数据集                                                                  */
        /* ====================================================================== */
        {
                .id = "zfs.create",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "create",
                .title = "创建数据集",
                .description = "创建文件系统；-p 创建父级，-u 不挂载，-o 设置属性，-n 演练。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-P", "-n", "-p", "-u", "-v"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.create.volume",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "create",
                .title = "创建 zvol",
                .description = "创建块设备卷；-V 指定大小，-b 块大小，-s 稀疏，-o 设置属性。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-P", "-n", "-p", "-s", "-v"),
                        .options = (const char *const[]){"o", "b", "V", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.destroy",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "destroy",
                .title = "销毁数据集/快照",
                .description = "删除数据集、卷或快照；-r 递归，-R 递归含克隆，-f 强制，-n 演练。",
                .category = ZG_CAT_DATASET,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-f", "-n", "-p", "-R", "-r", "-v"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.rename",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "rename",
                .title = "重命名",
                .description = "重命名数据集、卷或快照；-p 同时重命名父级，-u 不重新挂载，-r 递归。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-f", "-p", "-u", "-r"),
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_DATASET, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.clone",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "clone",
                .title = "克隆快照",
                .description = "以快照为源创建可写克隆数据集；-p 创建父级，-o 设置属性。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-p", "-u"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_SNAPSHOT, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.promote",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "promote",
                .title = "提升克隆",
                .description = "把克隆数据集提升为独立数据集，原快照关系反转。",
                .category = ZG_CAT_DATASET,
                .destructive = TRUE,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.mount",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "mount",
                .title = "挂载",
                .description = "挂载数据集；-a 全部，-R 递归，-o 挂载选项，-f 强制，-v 详细。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-a", "-f", "-l", "-v", "-O", "-R"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.unmount",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "unmount",
                .title = "卸载",
                .description = "卸载数据集；-a 全部，-f 强制，-u 延迟卸载。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-a", "-f", "-u"),
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.share",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "share",
                .title = "共享数据集",
                .description = "通过 NFS/SMB 共享数据集；-a 全部，-l 显示共享表。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-a", "-l"),
                        .min_positional = 0,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_DATASET, ZG_VALUE_FREE},
                },
        },
        {
                .id = "zfs.unshare",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "unshare",
                .title = "取消共享",
                .description = "取消数据集的 NFS/SMB 共享；-a 全部。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-a"),
                        .min_positional = 0,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_DATASET, ZG_VALUE_FREE},
                },
        },
        {
                .id = "zfs.jail",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "jail",
                .title = "挂接到 jail",
                .description = "把数据集绑定到指定 jail（FreeBSD 特有）。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.unjail",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "unjail",
                .title = "从 jail 解绑",
                .description = "解除数据集与 jail 的绑定（FreeBSD 特有）。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.upgrade",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "upgrade",
                .title = "升级数据集版本",
                .description = "升级数据集 on-disk 版本；-a 全部，-V 指定版本，-r 递归。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-a", "-r", "-v"),
                        .options = (const char *const[]){"V", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.rewrite",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "rewrite",
                .title = "重写数据块",
                .description = "重写指定范围内的块（用于修复/迁移）；-o 偏移，-l 长度。",
                .category = ZG_CAT_DATASET,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-P", "-r", "-v", "-x"),
                        .options = (const char *const[]){"o", "l", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_PATH},
                },
        },
        {
                .id = "zfs.wait",
                .tool = "zfs",
                .subcommand = "wait",
                .title = "等待数据集活动",
                .description = "等待数据集上的后台活动结束；-t 指定活动类型。",
                .category = ZG_CAT_DATASET,
                .long_running = TRUE,
                .spec = {
                        .options = (const char *const[]){"t", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },

        /* ====================================================================== */
        /* 数据集信息查询                                                          */
        /* ====================================================================== */
        {
                .id = "zfs.list",
                .tool = "zfs",
                .subcommand = "list",
                .title = "列出数据集",
                .description = "列出数据集/快照；-H 脚本输出，-p 精确数值，-j JSON，-t 类型过滤。",
                .category = ZG_CAT_DATASET,
                .spec = {
                        .flags = F("-H", "-p", "-j", "-r"),
                        .options = (const char *const[]){"o", "d", "s", "S", "t", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.get",
                .tool = "zfs",
                .subcommand = "get",
                .title = "读取属性",
                .description = "读取数据集/快照/池属性；-H 脚本输出，-p 精确数值，-s 来源过滤。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-H", "-p", "-j", "-r"),
                        .options = (const char *const[]){"o", "d", "t", "s", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.set",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "set",
                .title = "设置属性",
                .description = "设置一个或多个属性：property=value [dataset]。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_PROPERTY, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.inherit",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "inherit",
                .title = "继承属性",
                .description = "撤销本地属性值，回退到父级；-r 递归，-S 同时处理来源。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-r", "-S"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.userspace",
                .tool = "zfs",
                .subcommand = "userspace",
                .title = "用户空间占用",
                .description = "统计用户空间占用；-o 字段，-s/-S 排序，-t 类型，-i 显示 ID。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-H", "-i", "-n", "-p"),
                        .options = (const char *const[]){"o", "s", "S", "t", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.groupspace",
                .tool = "zfs",
                .subcommand = "groupspace",
                .title = "组空间占用",
                .description = "统计组空间占用；-o 字段，-s/-S 排序，-t 类型，-i 显示 ID。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-H", "-i", "-n", "-p"),
                        .options = (const char *const[]){"o", "s", "S", "t", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.projectspace",
                .tool = "zfs",
                .subcommand = "projectspace",
                .title = "项目空间占用",
                .description = "统计项目配额占用；-o 字段，-s/-S 排序。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-H", "-p"),
                        .options = (const char *const[]){"o", "s", "S", "t", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.project",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "project",
                .title = "项目配额管理",
                .description = "设置/清除目录的项目 ID；-p 项目 ID，-r 递归。",
                .category = ZG_CAT_PROPS,
                .spec = {
                        .flags = F("-c", "-C", "-d", "-r", "-k", "-0", "-s"),
                        .options = (const char *const[]){"p", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_PATH},
                },
        },

        /* ====================================================================== */
        /* 快照与书签                                                              */
        /* ====================================================================== */
        {
                .id = "zfs.snapshot",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "snapshot",
                .title = "创建快照",
                .description = "为一个或多个数据集创建快照；-r 递归，-o 设置属性。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .flags = F("-r"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.rollback",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "rollback",
                .title = "回滚到快照",
                .description = "丢弃快照之后的数据；-r 销毁更新的快照，-R 含克隆，-f 强制。",
                .category = ZG_CAT_SNAPSHOT,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-r", "-R", "-f"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.bookmark",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "bookmark",
                .title = "创建书签",
                .description = "把快照标记为书签，销毁快照后仍可用于增量发送。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_SNAPSHOT, ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.redact",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "redact",
                .title = "创建脱敏书签",
                .description = "生成可用于脱敏发送的 redaction 书签。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .min_positional = 3,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_SNAPSHOT, ZG_VALUE_SNAPSHOT, ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.diff",
                .tool = "zfs",
                .subcommand = "diff",
                .title = "比较快照差异",
                .description = "显示两个快照之间的文件差异；-H 脚本输出，-t 显示时间戳。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .flags = F("-F", "-H", "-t", "-h"),
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_SNAPSHOT, ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.hold",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "hold",
                .title = "持有快照",
                .description = "为快照添加 hold 标签，阻止被销毁；-r 递归。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .flags = F("-r"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_TAG, ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.holds",
                .tool = "zfs",
                .subcommand = "holds",
                .title = "查看快照 hold",
                .description = "列出快照上的 hold 标签；-H 脚本输出，-p 精确数值。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .flags = F("-H", "-p", "-r"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.release",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "release",
                .title = "释放快照 hold",
                .description = "移除快照上的 hold 标签；-r 递归。",
                .category = ZG_CAT_SNAPSHOT,
                .spec = {
                        .flags = F("-r"),
                        .min_positional = 2,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_TAG, ZG_VALUE_SNAPSHOT},
                },
        },

        /* ====================================================================== */
        /* 发送 / 接收                                                             */
        /* ====================================================================== */
        {
                .id = "zfs.send",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "send",
                .title = "发送快照",
                .description = "把快照流写入文件；-i/-I 增量，-R 复制属性与后代，-c 压缩，-n 演练。",
                .category = ZG_CAT_SENDRECV,
                .long_running = TRUE,
                .io = ZG_OP_IO_OUT_FILE,
                .io_arg = "output",
                .spec = {
                        .flags = F("-D", "-L", "-P", "-b", "-c", "-e", "-h", "-n", "-p", "-s", "-V", "-v", "-w", "-R"),
                        .options = (const char *const[]){"i", "I", "X", "t", NULL},
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_SNAPSHOT},
                },
        },
        {
                .id = "zfs.receive",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "receive",
                .title = "接收快照流",
                .description = "从文件读取快照流并写入数据集；-F 强制回滚，-n 演练，-u 不挂载。",
                .category = ZG_CAT_SENDRECV,
                .long_running = TRUE,
                .io = ZG_OP_IO_IN_FILE,
                .io_arg = "input",
                .spec = {
                        .flags = F("-v", "-M", "-n", "-s", "-F", "-h", "-u"),
                        .options = (const char *const[]){"o", "x", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },

        /* ====================================================================== */
        /* 加密                                                                    */
        /* ====================================================================== */
        {
                .id = "zfs.load-key",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "load-key",
                .title = "加载加密密钥",
                .description = "加载加密根数据集的密钥；-a 全部，-r 递归，-L 指定 keylocation。",
                .category = ZG_CAT_ENCRYPTION,
                .spec = {
                        .flags = F("-a", "-r", "-n"),
                        .options = (const char *const[]){"L", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.unload-key",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "zfs",
                .subcommand = "unload-key",
                .title = "卸载加密密钥",
                .description = "卸载密钥使数据集不可访问；-a 全部，-r 递归。",
                .category = ZG_CAT_ENCRYPTION,
                .spec = {
                        .flags = F("-a", "-r"),
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.change-key",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "change-key",
                .title = "更改密钥",
                .description = "更换加密密钥；-i 生成新密钥，-l 不重载，-o keyformat/keylocation。",
                .category = ZG_CAT_ENCRYPTION,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-i", "-l"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_DATASET},
                },
        },

        /* ====================================================================== */
        /* 委派权限                                                                */
        /* ====================================================================== */
        {
                .id = "zfs.allow",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "allow",
                .title = "授予委派权限",
                .description = "把 ZFS 管理权限委派给普通用户/组；-u/-g 用户或组，-d 后代，-s 权限集。",
                .category = ZG_CAT_DELEGATION,
                .spec = {
                        .flags = F("-l", "-d", "-u", "-g", "-e", "-c", "-s"),
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },
        {
                .id = "zfs.unallow",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "zfs",
                .subcommand = "unallow",
                .title = "撤销委派权限",
                .description = "撤销之前委派的权限；-r 递归。",
                .category = ZG_CAT_DELEGATION,
                .spec = {
                        .flags = F("-r", "-l", "-d", "-u", "-g", "-e", "-c", "-s"),
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE, ZG_VALUE_FREE, ZG_VALUE_DATASET},
                },
        },

        /* ====================================================================== */
        /* 启动环境（FreeBSD bectl）                                               */
        /* ====================================================================== */
        {
                .id = "bectl.list",
                .tool = "bectl",
                .subcommand = "list",
                .title = "列出启动环境",
                .description = "列出 ZFS 启动环境；-a 全部，-H 脚本输出，-s 显示空间。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .flags = F("-a", "-D", "-H", "-s"),
                        .options = (const char *const[]){"c", "C", NULL},
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_NAME},
                },
        },
        {
                .id = "bectl.create",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "create",
                .title = "创建启动环境",
                .description = "基于当前根创建启动环境；-r 递归，-e 来源 BE 或快照。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .flags = F("-r", "-E"),
                        .options = (const char *const[]){"e", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_NAME},
                },
        },
        {
                .id = "bectl.activate",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "activate",
                .title = "激活启动环境",
                .description = "切换下次启动使用的 BE；-t 临时（仅一次），-T 恢复默认。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .flags = F("-t", "-T"),
                        .min_positional = 0,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_NAME},
                },
        },
        {
                .id = "bectl.rename",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "rename",
                .title = "重命名启动环境",
                .description = "重命名启动环境。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .min_positional = 2,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_NAME, ZG_VALUE_NAME},
                },
        },
        {
                .id = "bectl.destroy",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "bectl",
                .subcommand = "destroy",
                .title = "销毁启动环境",
                .description = "删除启动环境；-F 强制，-o 指定来源。",
                .category = ZG_CAT_BOOTENV,
                .destructive = TRUE,
                .spec = {
                        .flags = F("-F"),
                        .options = (const char *const[]){"o", NULL},
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_NAME},
                },
        },
        {
                .id = "bectl.mount",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "mount",
                .title = "挂载启动环境",
                .description = "把启动环境挂载到指定路径以便修改。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_NAME, ZG_VALUE_PATH},
                },
        },
        {
                .id = "bectl.unmount",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "unmount",
                .title = "卸载启动环境",
                .description = "卸载已挂载的启动环境；-f 强制。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .flags = F("-f"),
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_PATH},
                },
        },
        {
                .id = "bectl.jail",
                .auth = ZG_AUTH_PASSWORD,
                .tool = "bectl",
                .subcommand = "jail",
                .title = "以 jail 运行启动环境",
                .description = "在 jail 中运行启动环境；-b 后台，-U 使用 jail 配置。",
                .category = ZG_CAT_BOOTENV,
                .long_running = TRUE,
                .spec = {
                        .flags = F("-b", "-U"),
                        .options = (const char *const[]){"o", "u", NULL},
                        .min_positional = 1,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_NAME, ZG_VALUE_FREE},
                },
        },
        {
                .id = "bectl.unjail",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "unjail",
                .title = "结束启动环境 jail",
                .description = "结束由 bectl jail 启动的 jail。",
                .category = ZG_CAT_BOOTENV,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 1,
                        .kinds = {ZG_VALUE_FREE},
                },
        },
        {
                .id = "bectl.export",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "export",
                .title = "导出启动环境",
                .description = "把启动环境导出为归档/发送流。",
                .category = ZG_CAT_BOOTENV,
                .long_running = TRUE,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_NAME, ZG_VALUE_PATH},
                },
        },
        {
                .id = "bectl.import",
                .auth = ZG_AUTH_CONFIRM,
                .tool = "bectl",
                .subcommand = "import",
                .title = "导入启动环境",
                .description = "从归档/流导入启动环境。",
                .category = ZG_CAT_BOOTENV,
                .long_running = TRUE,
                .spec = {
                        .min_positional = 1,
                        .max_positional = 2,
                        .kinds = {ZG_VALUE_PATH, ZG_VALUE_NAME},
                },
        },

        /* ====================================================================== */
        /* 系统信息                                                                */
        /* ====================================================================== */
        {
                .id = "system.info",
                .tool = NULL,
                .title = "系统与能力探测",
                .description = "返回 ZFS 版本、工具路径、JSON 输出支持、主机信息等。",
                .category = ZG_CAT_SYSTEM,
                .spec = {.max_positional = 0},
        },
        {
                .id = "system.arcstats",
                .tool = NULL,
                .title = "ARC 统计",
                .description = "读取 kstat.zfs.misc.arcstats（命中率、大小、回收等）。",
                .category = ZG_CAT_SYSTEM,
                .spec = {.max_positional = 0},
        },
        {
                .id = "system.disks",
                .tool = "geom",
                .subcommand = "disk list",
                .title = "列出磁盘",
                .description = "枚举系统中可用于创建池 / 替换设备的磁盘。",
                .category = ZG_CAT_SYSTEM,
                .spec = {.max_positional = 0},
        },
        {
                .id = "system.jails",
                .tool = "jls",
                .subcommand = NULL,
                .title = "列出 jail",
                .description = "列出正在运行的 jail（用于 zfs jail 操作）。",
                .category = ZG_CAT_SYSTEM,
                .spec = {
                        .flags = F("-h"),
                        .min_positional = 0,
                        .max_positional = 0,
                        .kinds = {ZG_VALUE_FREE},
                },
        },
        {
                .id = "zfs.version",
                .tool = "zfs",
                .subcommand = "version",
                .title = "ZFS 版本",
                .description = "显示用户态与内核 ZFS 版本。",
                .category = ZG_CAT_SYSTEM,
                .spec = {.max_positional = 0},
        },
};

static const char *const zg_registered_tools[] = {
        "zpool", "zfs", "bectl", "jls", "geom", NULL};

/* ------------------------------------------------------------------------- */
/* 查询接口                                                                   */
/* ------------------------------------------------------------------------- */

guint zg_op_count(void)
{
    return G_N_ELEMENTS(zg_operations);
}

const ZgOpDef *
zg_op_at(guint index)
{
    if (index >= G_N_ELEMENTS(zg_operations))
        return NULL;

    return &zg_operations[index];
}

const ZgOpDef *
zg_op_lookup(const char *id)
{
    guint i;

    if (id == NULL || id[0] == '\0')
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(zg_operations); i++)
    {
        if (g_strcmp0(zg_operations[i].id, id) == 0)
            return &zg_operations[i];
    }

    return NULL;
}

gboolean
zg_op_flag_allowed(const ZgOpDef *op,
                   const char *flag)
{
    guint i;

    if (op == NULL || flag == NULL || op->spec.flags == NULL)
        return FALSE;

    for (i = 0; op->spec.flags[i] != NULL; i++)
    {
        if (g_strcmp0(op->spec.flags[i], flag) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
zg_op_option_allowed(const ZgOpDef *op,
                     const char *key)
{
    guint i;

    if (op == NULL || key == NULL || op->spec.options == NULL)
        return FALSE;

    for (i = 0; op->spec.options[i] != NULL; i++)
    {
        if (g_strcmp0(op->spec.options[i], key) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
zg_op_is_registered(const char *tool)
{
    guint i;

    if (tool == NULL)
        return FALSE;

    for (i = 0; zg_registered_tools[i] != NULL; i++)
    {
        if (g_strcmp0(zg_registered_tools[i], tool) == 0)
            return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* 本地化标签                                                                 */
/* ------------------------------------------------------------------------- */

const char *
zg_op_category_label(ZgOpCategory category)
{
    static const char *const labels[ZG_CAT_COUNT] = {
            "存储池",
            "数据集",
            "快照",
            "属性",
            "发送/接收",
            "加密",
            "委派权限",
            "启动环境",
            "系统",
    };

    if ((guint)category >= G_N_ELEMENTS(labels))
        return "其他";

    return labels[category];
}

const char *
zg_op_category_id(ZgOpCategory category)
{
    static const char *const ids[ZG_CAT_COUNT] = {
            "pool",
            "dataset",
            "snapshot",
            "props",
            "sendrecv",
            "encryption",
            "delegation",
            "bootenv",
            "system",
    };

    if ((guint)category >= G_N_ELEMENTS(ids))
        return "other";

    return ids[category];
}

const char *
zg_auth_level_label(ZgAuthLevel level)
{
    switch (level)
    {
    case ZG_AUTH_PASSWORD:
        return "需要管理员密码";
    case ZG_AUTH_CONFIRM:
        return "需要确认";
    case ZG_AUTH_READ:
    default:
        return "只读";
    }
}

gboolean
zg_op_requires_password(const ZgOpDef *op,
                        const char *auth_mode)
{
    if (op == NULL)
        return FALSE;

    if (g_strcmp0(auth_mode, "none") == 0)
        return FALSE;

    if (g_strcmp0(auth_mode, "all") == 0)
        return op->auth >= ZG_AUTH_CONFIRM;

    /* 默认 "dangerous" */
    return op->auth == ZG_AUTH_PASSWORD;
}

const char *
zg_value_kind_label(ZgValueKind kind)
{
    switch (kind)
    {
    case ZG_VALUE_POOL:
        return "池名称";
    case ZG_VALUE_DATASET:
        return "数据集名称";
    case ZG_VALUE_SNAPSHOT:
        return "快照名称（dataset@snap）";
    case ZG_VALUE_VDEV:
        return "设备或 vdev 关键字（/dev/ada1、mirror …）";
    case ZG_VALUE_PATH:
        return "文件系统路径";
    case ZG_VALUE_NAME:
        return "名称";
    case ZG_VALUE_USER:
        return "用户";
    case ZG_VALUE_GROUP:
        return "组";
    case ZG_VALUE_TAG:
        return "标签";
    case ZG_VALUE_NUMBER:
        return "数字";
    case ZG_VALUE_SIZE:
        return "大小（如 10G）";
    case ZG_VALUE_PROPERTY:
        return "属性=值";
    case ZG_VALUE_FREE:
    default:
        return "值";
    }
}
