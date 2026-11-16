/* zg-opdef.h - 后端命令注册表（前端与守护进程共享）
 *
 * 设计目标
 * --------
 * 1. 前端不拼接命令行字符串，只提交「操作 ID + 位置参数 + 开关 + 选项」。
 * 2. 守护进程以本表为唯一白名单：未登记的操作、未登记的开关或选项一律拒绝。
 * 3. 执行时使用 execv()，不经过 shell，因此不存在命令注入。
 * 4. 表中只描述「调用形态」，不包含任何 ZFS 版本相关的特性判断，
 *    以便同一份二进制可在不同 FreeBSD / OpenZFS 版本上工作。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_OPDEF_H
#define ZG_OPDEF_H

#include <glib.h>

G_BEGIN_DECLS

/* 位置参数的类型，用于生成提示与做字符集校验 */
typedef enum
{
    ZG_VALUE_FREE,     /* 自由文本（不得以 '-' 开头） */
    ZG_VALUE_POOL,     /* 池名称 */
    ZG_VALUE_DATASET,  /* 数据集 / 卷 / 快照名 */
    ZG_VALUE_SNAPSHOT, /* 必须是 snapshot 或 bookmark 形式 */
    ZG_VALUE_VDEV,     /* 设备路径或 mirror/raidz 等关键字 */
    ZG_VALUE_PATH,     /* 文件系统路径 */
    ZG_VALUE_NAME,     /* 简单名称（BE 名称等） */
    ZG_VALUE_USER,     /* 用户名 / UID */
    ZG_VALUE_GROUP,    /* 组名 / GID */
    ZG_VALUE_TAG,      /* hold 标签 */
    ZG_VALUE_NUMBER,   /* 纯数字 */
    ZG_VALUE_SIZE,     /* 大小表达式，如 10G / 512M / 0 */
    ZG_VALUE_PROPERTY, /* property=value 或 property */
} ZgValueKind;

/* 操作的危险等级：决定是否需要二次确认 / 管理员密码 */
typedef enum
{
    ZG_AUTH_READ = 0, /* 只读，无需确认 */
    ZG_AUTH_CONFIRM,  /* 会修改状态，界面二次确认（不需要密码） */
    ZG_AUTH_PASSWORD, /* 危险操作，需要管理员密码重新验证 */
} ZgAuthLevel;

/* 命令的输入/输出重定向方式 */
typedef enum
{
    ZG_OP_IO_NONE,
    ZG_OP_IO_OUT_FILE, /* 子进程 stdout 写入 args[io_arg] 指定的文件 */
    ZG_OP_IO_IN_FILE,  /* 子进程 stdin 从 args[io_arg] 指定的文件读取 */
} ZgOpIo;

typedef enum
{
    ZG_CAT_POOL,
    ZG_CAT_DATASET,
    ZG_CAT_SNAPSHOT,
    ZG_CAT_PROPS,
    ZG_CAT_SENDRECV,
    ZG_CAT_ENCRYPTION,
    ZG_CAT_DELEGATION,
    ZG_CAT_BOOTENV,
    ZG_CAT_SYSTEM,
    ZG_CAT_COUNT,
} ZgOpCategory;

typedef struct
{
    const char *const *flags;   /* 允许的布尔开关，如 "-f"；NULL 结尾 */
    const char *const *options; /* 允许的带值选项键，如 "o"（即 -o VALUE） */
    guint min_positional;
    guint max_positional; /* 0 表示不限 */
    ZgValueKind kinds[4]; /* 超出 4 个时复用最后一个 */
} ZgOpSpec;

typedef struct
{
    const char *id;          /* "zpool.create" */
    const char *tool;        /* "zpool" / "zfs" / "bectl" / "jls" / NULL（内部实现） */
    const char *subcommand;  /* 可为 NULL 或多段（空格分隔） */
    const char *title;       /* 中文标题 */
    const char *description; /* 中文说明 */
    ZgOpCategory category;
    ZgAuthLevel auth;      /* 危险等级（默认 ZG_AUTH_READ） */
    gboolean destructive;  /* 破坏性操作，前端需要二次确认 */
    gboolean long_running; /* 可能长时间运行（scrub、receive 等） */
    ZgOpIo io;
    const char *io_arg; /* 重定向文件路径所在的参数名 */
    ZgOpSpec spec;
} ZgOpDef;

/* 注册表查询 */
const ZgOpDef *zg_op_lookup(const char *id);
guint zg_op_count(void);
const ZgOpDef *zg_op_at(guint index);
gboolean zg_op_flag_allowed(const ZgOpDef *op,
                            const char *flag);
gboolean zg_op_option_allowed(const ZgOpDef *op,
                              const char *key);
gboolean zg_op_is_registered(const char *tool);

/* 本地化标签 */
const char *zg_op_category_label(ZgOpCategory category);
const char *zg_op_category_id(ZgOpCategory category);
const char *zg_value_kind_label(ZgValueKind kind);
const char *zg_auth_level_label(ZgAuthLevel level);

/* 依据守护进程的 auth_mode 配置判断该操作是否需要密码：
 *   "dangerous"（默认）只对 ZG_AUTH_PASSWORD 要求密码
 *   "all"              对 CONFIRM 与 PASSWORD 都要求密码
 *   "none"             从不要求（不推荐） */
gboolean zg_op_requires_password(const ZgOpDef *op,
                                 const char *auth_mode);

G_END_DECLS

#endif /* ZG_OPDEF_H */
