/* zg-policy.h - 守护进程访问控制策略
 *
 * 配置文件 /usr/local/etc/zfs-gui.conf（key = value，# 开头为注释）
 *   allowed_users  = root, admin      # 允许的用户名
 *   allowed_groups = wheel, operator  # 允许的组名
 *   socket_path    = /var/run/zfs-gui/zfs-gui.sock
 *   socket_group   = wheel            # socket 文件属组
 *   socket_mode    = 0660             # socket 文件权限（八进制）
 *   io_timeout     = 600              # 普通操作超时（秒），0 表示不限
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_POLICY_H
#define ZG_POLICY_H

#include <glib.h>
#include <sys/types.h>

G_BEGIN_DECLS

typedef struct
{
    gchar **allowed_users;
    gchar **allowed_groups;
    gchar *socket_path;
    gchar *socket_group;
    guint socket_mode;
    guint io_timeout;
    gchar *auth_mode;        /* dangerous / all / none */
    guint auth_max_attempts; /* 连续失败上限 */
    guint auth_lockout;      /* 锁定时长（秒） */
} ZgPolicy;

/* 读取配置；文件不存在时使用安全默认值（root + wheel 组） */
ZgPolicy *zg_policy_load(const gchar *path,
                         GError **error);
void zg_policy_free(ZgPolicy *policy);

/* 依据 getpeereid() 得到的身份做鉴权 */
gboolean zg_policy_authorize(const ZgPolicy *policy,
                             uid_t uid,
                             gid_t gid,
                             gchar **message);

/* socket 属组解析，失败返回 (gid_t)-1 */
gid_t zg_policy_socket_gid(const ZgPolicy *policy);

const gchar *zg_policy_socket_path(const ZgPolicy *policy);

/* 认证策略："dangerous"（默认）/ "all" / "none" */
const gchar *zg_policy_auth_mode(const ZgPolicy *policy);

/* 该用户名是否属于管理员集合（root / allowed_users / allowed_groups） */
gboolean zg_policy_user_is_admin(const ZgPolicy *policy,
                                 const gchar *username);

G_END_DECLS

#endif /* ZG_POLICY_H */
