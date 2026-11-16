/* zg-util.c - 通用工具实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-util.h"

#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------- */
/* 字符串                                                                     */
/* ------------------------------------------------------------------------- */

gboolean
zg_str_is_empty(const char *str)
{
    return str == NULL || str[0] == '\0';
}

const char *
zg_str_or(const char *str,
          const char *fallback)
{
    return zg_str_is_empty(str) ? fallback : str;
}

gchar *
zg_str_trim(const char *str)
{
    gchar *copy;
    gchar *start;

    if (str == NULL)
        return g_strdup("");

    copy = g_strdup(str);
    start = g_strstrip(copy);

    if (start != copy)
        memmove(copy, start, strlen(start) + 1);

    return copy;
}

gchar *
zg_str_join(GPtrArray *items,
            const char *separator)
{
    GString *buffer;
    guint i;

    if (items == NULL || items->len == 0)
        return g_strdup("");

    buffer = g_string_new(NULL);

    for (i = 0; i < items->len; i++)
    {
        const char *item = g_ptr_array_index(items, i);

        if (i > 0)
            g_string_append(buffer, separator);

        if (item != NULL)
            g_string_append(buffer, item);
    }

    return g_string_free(buffer, FALSE);
}

/* ------------------------------------------------------------------------- */
/* 数值 / 时间                                                                */
/* ------------------------------------------------------------------------- */

gchar *
zg_format_bytes(guint64 bytes)
{
    static const char units[] = {'K', 'M', 'G', 'T', 'P', 'E'};
    gdouble value = (gdouble)bytes;
    gint unit = -1;
    gchar *number;
    gsize length;

    while (value >= 1024.0 && unit < (gint)G_N_ELEMENTS(units) - 1)
    {
        value /= 1024.0;
        unit++;
    }

    if (unit < 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT "B", bytes);

    if (value < 10.0)
        number = g_strdup_printf("%.2f", value);
    else if (value < 100.0)
        number = g_strdup_printf("%.1f", value);
    else
        number = g_strdup_printf("%.0f", value);

    /* 去掉无意义的尾零：1.50 -> 1.5，1.00 -> 1 */
    length = strlen(number);

    while (length > 0 && number[length - 1] == '0')
        number[--length] = '\0';

    if (length > 0 && number[length - 1] == '.')
        number[--length] = '\0';

    {
        gchar *result = g_strdup_printf("%s%c", number, units[unit]);

        g_free(number);

        return result;
    }
}

gchar *
zg_format_epoch_local(gint64 epoch)
{
    struct tm tm;
    time_t when = (time_t)epoch;
    char buffer[64];

    if (epoch <= 0)
        return g_strdup("—");

    if (localtime_r(&when, &tm) == NULL)
        return g_strdup("—");

    if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm) == 0)
        return g_strdup("—");

    return g_strdup(buffer);
}

gchar *
zg_format_epoch_relative(gint64 epoch)
{
    gint64 now;
    gint64 delta;
    gint64 amount;
    const char *suffix;

    if (epoch <= 0)
        return g_strdup("—");

    now = (gint64)time(NULL);
    delta = now - epoch;

    if (delta < 0)
        return g_strdup("刚刚");

    if (delta < 60)
    {
        amount = delta;
        suffix = "秒前";
    }
    else if (delta < 3600)
    {
        amount = delta / 60;
        suffix = "分钟前";
    }
    else if (delta < 86400)
    {
        amount = delta / 3600;
        suffix = "小时前";
    }
    else if (delta < 86400 * 30)
    {
        amount = delta / 86400;
        suffix = "天前";
    }
    else if (delta < 86400 * 365)
    {
        amount = delta / (86400 * 30);
        suffix = "个月前";
    }
    else
    {
        amount = delta / (86400 * 365);
        suffix = "年前";
    }

    return g_strdup_printf("%" G_GINT64_FORMAT " %s", amount, suffix);
}

gboolean
zg_parse_u64(const char *str,
             guint64 *value_out)
{
    guint64 value = 0;

    if (str == NULL || str[0] == '\0')
        return FALSE;

    if (!g_ascii_string_to_unsigned(str, 10, 0, G_MAXUINT64, &value, NULL))
        return FALSE;

    if (value_out != NULL)
        *value_out = value;

    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* 名称校验                                                                   */
/* ------------------------------------------------------------------------- */

/* 通用检查：非空、不以 '-' 开头、长度受限、不含控制字符 */
static gboolean
name_prefix_ok(const char *name)
{
    const char *p;

    if (name == NULL || name[0] == '\0')
        return FALSE;

    if (name[0] == '-')
        return FALSE;

    if (strlen(name) > 1024)
        return FALSE;

    for (p = name; *p != '\0'; p++)
    {
        if ((guchar)*p < 0x20 || *p == 0x7f)
            return FALSE;
    }

    return TRUE;
}

static gboolean
name_charset_ok(const char *name,
                const char *extra)
{
    const char *p;

    for (p = name; *p != '\0'; p++)
    {
        if (g_ascii_isalnum(*p))
            continue;

        if (strchr("_.:-", *p) != NULL)
            continue;

        if (extra != NULL && strchr(extra, *p) != NULL)
            continue;

        return FALSE;
    }

    return TRUE;
}

gboolean
zg_pool_name_valid(const char *name)
{
    if (!name_prefix_ok(name))
        return FALSE;

    if (strchr(name, '/') != NULL)
        return FALSE;

    return name_charset_ok(name, "");
}

gboolean
zg_dataset_name_valid(const char *name)
{
    const char *at;

    if (!name_prefix_ok(name))
        return FALSE;

    if (!name_charset_ok(name, "/@#%+ "))
        return FALSE;

    /* 数据集名称不能包含空格（ZFS 本身不允许） */
    if (strchr(name, ' ') != NULL)
        return FALSE;

    /* 至少包含一个非分隔字符 */
    at = strpbrk(name, "@#");
    if (at != NULL && at == name)
        return FALSE;

    return TRUE;
}

gboolean
zg_snapshot_name_valid(const char *name)
{
    const char *separator;

    if (!zg_dataset_name_valid(name))
        return FALSE;

    separator = strpbrk(name, "@#");

    if (separator == NULL)
        return FALSE;

    if (separator == name || separator[1] == '\0')
        return FALSE;

    return TRUE;
}

gboolean
zg_vdev_spec_valid(const char *spec)
{
    static const char *const keywords[] = {
            "mirror",
            "raidz",
            "raidz1",
            "raidz2",
            "raidz3",
            "draid",
            "draid1",
            "draid2",
            "draid3",
            "spare",
            "log",
            "cache",
            "special",
            "dedup",
            "indirect",
            "replacing",
            "stripe",
    };
    guint i;
    const char *p;

    if (!name_prefix_ok(spec))
        return FALSE;

    for (i = 0; i < G_N_ELEMENTS(keywords); i++)
    {
        if (g_strcmp0(spec, keywords[i]) == 0)
            return TRUE;
    }

    if (g_str_has_prefix(spec, "raidz") || g_str_has_prefix(spec, "draid"))
        return TRUE;

    /* 设备路径：/dev/ada0p3、/dev/gpt/disk1、nda0p3、file:/path 等 */
    for (p = spec; *p != '\0'; p++)
    {
        if (g_ascii_isalnum(*p))
            continue;
        if (strchr("_.:/@+-", *p) != NULL)
            continue;
        return FALSE;
    }

    return TRUE;
}

gboolean
zg_simple_name_valid(const char *name)
{
    if (!name_prefix_ok(name))
        return FALSE;

    if (strchr(name, '/') != NULL || strchr(name, ' ') != NULL)
        return FALSE;

    return name_charset_ok(name, "+");
}

gboolean
zg_user_name_valid(const char *name)
{
    if (!name_prefix_ok(name))
        return FALSE;

    if (strchr(name, '/') != NULL || strchr(name, ' ') != NULL)
        return FALSE;

    return name_charset_ok(name, "+$");
}
