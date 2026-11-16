/* zg-policy.c - 守护进程访问控制策略实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-policy.h"

/* 默认 socket 权限：属主与属组可读写 */
#define ZG_SOCKET_MODE_DEFAULT 0660

#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "zg-protocol.h"
#include "zg-util.h"

static gchar **
split_list(const gchar *value)
{
    gchar **parts;
    GPtrArray *items;
    guint i;

    items = g_ptr_array_new_with_free_func(g_free);
    parts = g_strsplit(value, ",", -1);

    for (i = 0; parts[i] != NULL; i++)
    {
        gchar *item = zg_str_trim(parts[i]);

        if (item[0] != '\0')
            g_ptr_array_add(items, item);
        else
            g_free(item);
    }

    g_strfreev(parts);

    g_ptr_array_add(items, NULL);

    return (gchar **)g_ptr_array_free(items, FALSE);
}

static gchar **
list_from_default(const gchar *default_value)
{
    return split_list(default_value);
}

ZgPolicy *
zg_policy_load(const gchar *path,
               GError **error)
{
    ZgPolicy *policy = g_new0(ZgPolicy, 1);
    gchar *contents = NULL;
    gchar **lines;
    guint i;

    policy->allowed_users = list_from_default("root");
    policy->allowed_groups = list_from_default("wheel");
    policy->socket_path = g_strdup(ZG_SOCKET_PATH_DEFAULT);
    policy->socket_group = g_strdup("wheel");
    policy->socket_mode = ZG_SOCKET_MODE_DEFAULT;
    policy->io_timeout = ZG_IO_TIMEOUT_SECONDS;
    policy->auth_mode = g_strdup("dangerous");
    policy->strict_mode = FALSE;
    policy->capsicum_mode = FALSE;
    policy->auth_max_attempts = 3;
    policy->auth_lockout = 300;

    if (path == NULL)
        path = ZG_CONFIG_PATH_DEFAULT;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return policy; /* 使用默认值 */

    lines = g_strsplit(contents, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar *line = zg_str_trim(lines[i]);
        gchar *equals;
        gchar *key;
        gchar *value;

        if (line[0] == '\0' || line[0] == '#')
        {
            g_free(line);
            continue;
        }

        equals = strchr(line, '=');

        if (equals == NULL)
        {
            g_free(line);
            continue;
        }

        *equals = '\0';
        key = zg_str_trim(line);
        value = zg_str_trim(equals + 1);

        if (g_strcmp0(key, "allowed_users") == 0)
        {
            g_strfreev(policy->allowed_users);
            policy->allowed_users = split_list(value);
        }
        else if (g_strcmp0(key, "allowed_groups") == 0)
        {
            g_strfreev(policy->allowed_groups);
            policy->allowed_groups = split_list(value);
        }
        else if (g_strcmp0(key, "socket_path") == 0)
        {
            g_free(policy->socket_path);
            policy->socket_path = g_strdup(value);
        }
        else if (g_strcmp0(key, "socket_group") == 0)
        {
            g_free(policy->socket_group);
            policy->socket_group = g_strdup(value);
        }
        else if (g_strcmp0(key, "socket_mode") == 0)
        {
            guint64 mode = 0;

            if (g_ascii_string_to_unsigned(value, 8, 0, 0777, &mode, NULL))
                policy->socket_mode = (guint)mode;
        }
        else if (g_strcmp0(key, "io_timeout") == 0)
        {
            guint64 timeout = 0;

            if (g_ascii_string_to_unsigned(value, 10, 0, G_MAXUINT, &timeout, NULL))
                policy->io_timeout = (guint)timeout;
        }
        else if (g_strcmp0(key, "capsicum_mode") == 0)
        {
            if (g_ascii_strcasecmp(value, "on") == 0 ||
                g_ascii_strcasecmp(value, "yes") == 0 ||
                g_ascii_strcasecmp(value, "true") == 0 ||
                g_strcmp0(value, "1") == 0)
                policy->capsicum_mode = TRUE;
            else if (g_ascii_strcasecmp(value, "off") == 0 ||
                     g_ascii_strcasecmp(value, "no") == 0 ||
                     g_ascii_strcasecmp(value, "false") == 0 ||
                     g_strcmp0(value, "0") == 0)
                policy->capsicum_mode = FALSE;
            else
            {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "capsicum_mode 取值非法：%s（应为 on/off）", value);
                return NULL;
            }
        }
        else if (g_strcmp0(key, "strict_mode") == 0)
        {
            if (g_ascii_strcasecmp(value, "on") == 0 ||
                g_ascii_strcasecmp(value, "yes") == 0 ||
                g_ascii_strcasecmp(value, "true") == 0 ||
                g_strcmp0(value, "1") == 0)
                policy->strict_mode = TRUE;
            else if (g_ascii_strcasecmp(value, "off") == 0 ||
                     g_ascii_strcasecmp(value, "no") == 0 ||
                     g_ascii_strcasecmp(value, "false") == 0 ||
                     g_strcmp0(value, "0") == 0)
                policy->strict_mode = FALSE;
            else
            {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "strict_mode 取值非法：%s（应为 on/off）", value);
                return NULL;
            }
        }
        else if (g_strcmp0(key, "auth_mode") == 0)
        {
            if (g_strcmp0(value, "dangerous") == 0 ||
                g_strcmp0(value, "all") == 0 ||
                g_strcmp0(value, "none") == 0)
            {
                g_free(policy->auth_mode);
                if (policy->allowed_uids != NULL)
                    g_array_free(policy->allowed_uids, TRUE);
                policy->auth_mode = g_strdup(value);
            }
        }
        else if (g_strcmp0(key, "auth_max_attempts") == 0)
        {
            guint64 attempts = 0;

            if (g_ascii_string_to_unsigned(value, 10, 1, 100, &attempts, NULL))
                policy->auth_max_attempts = (guint)attempts;
        }
        else if (g_strcmp0(key, "auth_lockout") == 0)
        {
            guint64 lockout = 0;

            if (g_ascii_string_to_unsigned(value, 10, 0, 86400, &lockout, NULL))
                policy->auth_lockout = (guint)lockout;
        }

        g_free(key);
        g_free(value);
        g_free(line);
    }

    g_strfreev(lines);
    g_free(contents);

    if (error != NULL)
        *error = NULL;

    return policy;
}

void zg_policy_free(ZgPolicy *policy)
{
    if (policy == NULL)
        return;

    g_strfreev(policy->allowed_users);
    g_strfreev(policy->allowed_groups);
    g_free(policy->socket_path);
    g_free(policy->socket_group);
    g_free(policy->auth_mode);
    if (policy->allowed_uids != NULL)
        g_array_free(policy->allowed_uids, TRUE);
    g_free(policy);
}

static gboolean
list_contains(gchar **list,
              const gchar *value)
{
    guint i;

    if (list == NULL || value == NULL)
        return FALSE;

    for (i = 0; list[i] != NULL; i++)
    {
        if (g_strcmp0(list[i], value) == 0)
            return TRUE;
    }

    return FALSE;
}

gid_t zg_policy_socket_gid(const ZgPolicy *policy)
{
    struct group *group;

    if (policy == NULL || zg_str_is_empty(policy->socket_group))
        return (gid_t)-1;

    group = getgrnam(policy->socket_group);

    if (group == NULL)
        return (gid_t)-1;

    return group->gr_gid;
}

gboolean
zg_policy_strict_mode(const ZgPolicy *policy)
{
    return policy != NULL && policy->strict_mode;
}

gboolean
zg_policy_capsicum_mode(const ZgPolicy *policy)
{
    return policy != NULL && policy->capsicum_mode;
}

const gchar *
zg_policy_auth_mode(const ZgPolicy *policy)
{
    if (policy == NULL || zg_str_is_empty(policy->auth_mode))
        return "dangerous";

    return policy->auth_mode;
}

gboolean
zg_policy_user_is_admin(const ZgPolicy *policy,
                        const gchar *username)
{
    struct passwd *pw;
    gid_t *groups;
    gint ngroups = 16;
    gboolean admin = FALSE;
    gint i;

    if (policy == NULL || username == NULL || username[0] == '\0')
        return FALSE;

    if (g_strcmp0(username, "root") == 0)
        return TRUE;

    if (list_contains(policy->allowed_users, username))
        return TRUE;

    pw = getpwnam(username);

    if (pw == NULL)
        return FALSE;

    groups = g_new0(gid_t, (gsize)ngroups);

    if (getgrouplist(username, pw->pw_gid, groups, &ngroups) < 0)
    {
        g_free(groups);
        groups = g_new0(gid_t, (gsize)ngroups);

        if (getgrouplist(username, pw->pw_gid, groups, &ngroups) < 0)
        {
            g_free(groups);
            return FALSE;
        }
    }

    for (i = 0; i < ngroups; i++)
    {
        struct group *gr = getgrgid(groups[i]);

        if (gr == NULL)
            continue;

        if (list_contains(policy->allowed_groups, gr->gr_name))
        {
            admin = TRUE;
            break;
        }
    }

    g_free(groups);

    return admin;
}

const gchar *
zg_policy_socket_path(const ZgPolicy *policy)
{
    if (policy == NULL || zg_str_is_empty(policy->socket_path))
        return ZG_SOCKET_PATH_DEFAULT;

    return policy->socket_path;
}

gboolean
zg_policy_precompute_identities(ZgPolicy *policy)
{
    GArray *uids;
    GHashTable *gids;
    struct passwd *pw;
    guint i;

    if (policy == NULL)
        return FALSE;

    uids = g_array_new(FALSE, FALSE, sizeof(uid_t));
    gids = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (i = 0; policy->allowed_groups != NULL &&
                policy->allowed_groups[i] != NULL;
         i++)
    {
        struct group *gr = getgrnam(policy->allowed_groups[i]);

        if (gr != NULL)
            g_hash_table_add(gids, GINT_TO_POINTER((gint)gr->gr_gid));
    }

    for (i = 0; policy->allowed_users != NULL &&
                policy->allowed_users[i] != NULL;
         i++)
    {
        pw = getpwnam(policy->allowed_users[i]);

        if (pw != NULL)
        {
            uid_t uid = pw->pw_uid;

            g_array_append_val(uids, uid);
        }
    }

    setpwent();

    while ((pw = getpwent()) != NULL)
    {
        gid_t groups[NGROUPS_MAX];
        gint ngroups = NGROUPS_MAX;
        gint j;

        if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
            continue;

        for (j = 0; j < ngroups; j++)
        {
            if (g_hash_table_contains(gids, GINT_TO_POINTER((gint)groups[j])))
            {
                uid_t uid = pw->pw_uid;
                gboolean found = FALSE;
                guint k;

                for (k = 0; k < uids->len; k++)
                {
                    if (uid == g_array_index(uids, uid_t, k))
                    {
                        found = TRUE;
                        break;
                    }
                }

                if (!found)
                    g_array_append_val(uids, uid);
                break;
            }
        }
    }

    endpwent();
    g_hash_table_destroy(gids);

    if (policy->allowed_uids != NULL)
        g_array_free(policy->allowed_uids, TRUE);
    policy->allowed_uids = uids;

    return TRUE;
}

gboolean
zg_policy_authorize(const ZgPolicy *policy,
                    uid_t uid,
                    gid_t gid,
                    gchar **message)
{
    struct passwd *pw;
    gid_t *groups = NULL;
    gint ngroups = 16;
    gboolean allowed = FALSE;

    if (message != NULL)
        *message = NULL;

    if (policy == NULL)
        return FALSE;

    /* root 始终允许 */
    if (uid == 0)
        return TRUE;

    /* 能力模式：使用启动时预解析的 UID 列表，避免 passwd/group 查找 */
    if (policy->allowed_uids != NULL)
    {
        guint i;

        for (i = 0; i < policy->allowed_uids->len; i++)
        {
            if (uid == g_array_index(policy->allowed_uids, uid_t, i))
            {
                if (message != NULL)
                    *message = g_strdup("已通过预解析的 allowed_users/allowed_groups 授权");
                return TRUE;
            }
        }

        if (message != NULL)
            *message = g_strdup("用户不在允许列表内（allowed_users / allowed_groups）");
        return FALSE;
    }

    pw = getpwuid(uid);

    if (pw == NULL)
    {
        if (message != NULL)
            *message = g_strdup_printf("无法解析 UID %u", (guint)uid);
        return FALSE;
    }

    if (list_contains(policy->allowed_users, pw->pw_name))
    {
        if (message != NULL)
            *message = g_strdup_printf("用户 %s 已通过 allowed_users 授权", pw->pw_name);
        return TRUE;
    }

    groups = g_new0(gid_t, (gsize)ngroups);

    if (getgrouplist(pw->pw_name, (gid_t)gid, groups, &ngroups) < 0)
    {
        /* 缓冲区不足，ngroups 已被更新为所需大小 */
        g_free(groups);
        groups = g_new0(gid_t, (gsize)ngroups);

        if (getgrouplist(pw->pw_name, (gid_t)gid, groups, &ngroups) < 0)
        {
            g_free(groups);
            if (message != NULL)
                *message = g_strdup_printf("无法读取用户 %s 的组成员关系", pw->pw_name);
            return FALSE;
        }
    }

    {
        gint i;

        for (i = 0; i < ngroups; i++)
        {
            struct group *gr = getgrgid(groups[i]);

            if (gr == NULL)
                continue;

            if (list_contains(policy->allowed_groups, gr->gr_name))
            {
                allowed = TRUE;
                break;
            }
        }
    }

    g_free(groups);

    if (!allowed && message != NULL)
    {
        *message = g_strdup_printf(
                "用户 %s 不在允许列表内（allowed_users / allowed_groups）。\n"
                "请编辑 " ZG_CONFIG_PATH_DEFAULT " 后重启 zfsguid。",
                pw->pw_name);
    }
    else if (allowed && message != NULL)
    {
        *message = g_strdup("授权通过");
    }

    return allowed;
}
