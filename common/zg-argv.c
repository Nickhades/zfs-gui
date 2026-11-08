/* zg-argv.c - 请求解析、白名单校验与 argv 组装实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-argv.h"

#include <string.h>

#include "zg-protocol.h"
#include "zg-util.h"

#define ZG_MAX_ARG_LENGTH 4096
#define ZG_MAX_OPTION_KEY_LENGTH 16

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

static void
option_free(gpointer data)
{
    ZgOpOption *option = data;

    if (option == NULL)
        return;

    g_free(option->key);
    g_free(option->value);
    g_free(option);
}

ZgOpRequest *
zg_op_request_new(void)
{
    ZgOpRequest *request = g_new0(ZgOpRequest, 1);

    request->flags = g_ptr_array_new_with_free_func(g_free);
    request->options = g_ptr_array_new_with_free_func(option_free);
    request->positional = g_ptr_array_new_with_free_func(g_free);

    return request;
}

void zg_op_request_free(ZgOpRequest *request)
{
    if (request == NULL)
        return;

    g_ptr_array_free(request->flags, TRUE);
    g_ptr_array_free(request->options, TRUE);
    g_ptr_array_free(request->positional, TRUE);
    g_free(request->io_path);
    g_free(request);
}

/* ------------------------------------------------------------------------- */
/* 解析                                                                       */
/* ------------------------------------------------------------------------- */

static gboolean
parse_flags(const ZgJson *flags, ZgOpRequest *request, GError **error)
{
    guint i;

    if (flags == NULL || zg_json_is_null(flags))
        return TRUE;

    if (zg_json_get_type(flags) != ZG_JSON_TYPE_ARRAY)
    {
        g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                            "args.flags 必须是字符串数组");
        return FALSE;
    }

    for (i = 0; i < zg_json_array_size(flags); i++)
    {
        const char *flag = zg_json_get_string(zg_json_array_at(flags, i));

        if (flag == NULL || flag[0] != '-')
        {
            g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                                "args.flags 中的开关必须以 '-' 开头");
            return FALSE;
        }

        g_ptr_array_add(request->flags, g_strdup(flag));
    }

    return TRUE;
}

static gboolean
parse_options(const ZgJson *options, ZgOpRequest *request, GError **error)
{
    guint i;

    if (options == NULL || zg_json_is_null(options))
        return TRUE;

    if (zg_json_get_type(options) != ZG_JSON_TYPE_ARRAY)
    {
        g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                            "args.options 必须是对象数组");
        return FALSE;
    }

    for (i = 0; i < zg_json_array_size(options); i++)
    {
        const ZgJson *item = zg_json_array_at(options, i);
        const char *key;
        const char *value;
        ZgOpOption *option;

        if (zg_json_get_type(item) != ZG_JSON_TYPE_OBJECT)
        {
            g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                                "args.options 的元素必须是 {key,value} 对象");
            return FALSE;
        }

        key = zg_json_object_get_string(item, ZG_KEY_KEY);
        value = zg_json_object_get_string(item, ZG_KEY_VALUE);

        if (zg_str_is_empty(key) || value == NULL)
        {
            g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                                "args.options 的元素缺少 key 或 value");
            return FALSE;
        }

        option = g_new0(ZgOpOption, 1);
        option->key = g_strdup(key);
        option->value = g_strdup(value);
        g_ptr_array_add(request->options, option);
    }

    return TRUE;
}

static gboolean
parse_positional(const ZgJson *positional, ZgOpRequest *request, GError **error)
{
    guint i;

    if (positional == NULL || zg_json_is_null(positional))
        return TRUE;

    if (zg_json_get_type(positional) != ZG_JSON_TYPE_ARRAY)
    {
        g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                            "args.positional 必须是字符串数组");
        return FALSE;
    }

    for (i = 0; i < zg_json_array_size(positional); i++)
    {
        const char *value = zg_json_get_string(zg_json_array_at(positional, i));

        if (value == NULL)
        {
            g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                                "args.positional 的元素必须是字符串");
            return FALSE;
        }

        g_ptr_array_add(request->positional, g_strdup(value));
    }

    return TRUE;
}

gboolean
zg_op_request_parse(const ZgJson *args,
                    ZgOpRequest *request,
                    GError **error)
{
    const ZgJson *node;

    g_return_val_if_fail(request != NULL, FALSE);

    if (args == NULL || zg_json_is_null(args))
        return TRUE;

    if (zg_json_get_type(args) != ZG_JSON_TYPE_OBJECT)
    {
        g_set_error_literal(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                            "args 必须是对象");
        return FALSE;
    }

    if (!parse_flags(zg_json_object_get(args, ZG_KEY_FLAGS), request, error))
        return FALSE;

    if (!parse_options(zg_json_object_get(args, ZG_KEY_OPTIONS), request, error))
        return FALSE;

    if (!parse_positional(zg_json_object_get(args, ZG_KEY_POSITIONAL),
                          request, error))
        return FALSE;

    node = zg_json_object_get(args, "output");
    if (zg_json_get_type(node) == ZG_JSON_TYPE_STRING)
        request->io_path = g_strdup(zg_json_get_string(node));

    node = zg_json_object_get(args, "input");
    if (zg_json_get_type(node) == ZG_JSON_TYPE_STRING)
    {
        g_free(request->io_path);
        request->io_path = g_strdup(zg_json_get_string(node));
    }

    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* 校验                                                                       */
/* ------------------------------------------------------------------------- */

gboolean
zg_value_valid(ZgValueKind kind,
               const char *value)
{
    guint64 number = 0;

    if (value == NULL)
        return FALSE;

    if (strlen(value) > ZG_MAX_ARG_LENGTH)
        return FALSE;

    switch (kind)
    {
    case ZG_VALUE_POOL:
        return zg_pool_name_valid(value);

    case ZG_VALUE_DATASET:
        return zg_dataset_name_valid(value);

    case ZG_VALUE_SNAPSHOT:
        return zg_snapshot_name_valid(value);

    case ZG_VALUE_VDEV:
        return zg_vdev_spec_valid(value);

    case ZG_VALUE_NAME:
    case ZG_VALUE_TAG:
        return zg_simple_name_valid(value);

    case ZG_VALUE_USER:
    case ZG_VALUE_GROUP:
        return zg_user_name_valid(value);

    case ZG_VALUE_PATH:
        return value[0] != '-' && value[0] != '\0';

    case ZG_VALUE_NUMBER:
        return zg_parse_u64(value, &number);

    case ZG_VALUE_SIZE:
        /* 允许 10G / 512M / 1024 / 0 / 50% 等 */
        return value[0] != '-' && value[0] != '\0';

    case ZG_VALUE_PROPERTY:
        /* property=value 或裸属性名 */
        return value[0] != '-' && value[0] != '\0';

    case ZG_VALUE_FREE:
    default:
        return value[0] != '-' && value[0] != '\0';
    }
}

/* 属性名白名单字符集：允许 feature@、user: 等前缀 */
static gboolean
property_key_valid(const char *key)
{
    const char *p;

    if (key == NULL || key[0] == '\0')
        return FALSE;

    if (!g_ascii_isalpha(key[0]))
        return FALSE;

    for (p = key; *p != '\0'; p++)
    {
        if (g_ascii_isalnum(*p) || strchr("_.:@-", *p) != NULL)
            continue;
        return FALSE;
    }

    return TRUE;
}

static gboolean
option_value_valid(const char *key,
                   const char *value)
{
    const char *equals;

    if (value == NULL || value[0] == '\0')
        return FALSE;

    if (strlen(value) > ZG_MAX_ARG_LENGTH)
        return FALSE;

    /* 带值选项的值不能以 '-' 开头，否则可能被当成另一个开关 */
    if (value[0] == '-')
        return FALSE;

    if (strchr(value, '\n') != NULL || strchr(value, '\r') != NULL)
        return FALSE;

    if (g_strcmp0(key, "o") == 0 || g_strcmp0(key, "O") == 0)
    {
        equals = strchr(value, '=');

        if (equals != NULL)
        {
            char *property;
            gboolean valid;

            if (equals == value)
                return FALSE;

            property = g_strndup(value, (gsize)(equals - value));
            valid = property_key_valid(property);
            g_free(property);

            return valid;
        }

        /* -o 也可用于选择输出字段：zfs list -o name,used,avail */
        {
            const char *p;

            if (!g_ascii_isalnum(value[0]))
                return FALSE;

            for (p = value; *p != '\0'; p++)
            {
                if (g_ascii_isalnum(*p) || strchr("_,.:@", *p) != NULL)
                    continue;
                return FALSE;
            }

            return TRUE;
        }
    }

    if (g_strcmp0(key, "x") == 0)
        return property_key_valid(value);

    return TRUE;
}

gboolean
zg_op_request_validate(const ZgOpDef *op,
                       const ZgOpRequest *request,
                       GError **error)
{
    guint i;

    g_return_val_if_fail(op != NULL, FALSE);
    g_return_val_if_fail(request != NULL, FALSE);

    for (i = 0; i < request->flags->len; i++)
    {
        const char *flag = g_ptr_array_index(request->flags, i);

        if (!zg_op_flag_allowed(op, flag))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 不允许开关 %s", op->id, flag);
            return FALSE;
        }
    }

    for (i = 0; i < request->options->len; i++)
    {
        const ZgOpOption *option = g_ptr_array_index(request->options, i);

        if (strlen(option->key) > ZG_MAX_OPTION_KEY_LENGTH ||
            !g_ascii_isalnum(option->key[0]))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 的选项键非法：%s", op->id, option->key);
            return FALSE;
        }

        if (!zg_op_option_allowed(op, option->key))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 不允许选项 -%s", op->id, option->key);
            return FALSE;
        }

        if (!option_value_valid(option->key, option->value))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 的选项 -%s 取值非法", op->id, option->key);
            return FALSE;
        }
    }

    if (request->positional->len < op->spec.min_positional)
    {
        g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                    "操作 %s 至少需要 %u 个参数，实际 %u 个",
                    op->id, op->spec.min_positional, request->positional->len);
        return FALSE;
    }

    if (op->spec.max_positional > 0 &&
        request->positional->len > op->spec.max_positional)
    {
        g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                    "操作 %s 最多接受 %u 个参数，实际 %u 个",
                    op->id, op->spec.max_positional, request->positional->len);
        return FALSE;
    }

    for (i = 0; i < request->positional->len; i++)
    {
        const char *value = g_ptr_array_index(request->positional, i);
        const ZgValueKind kind =
                (i < G_N_ELEMENTS(op->spec.kinds)) ? op->spec.kinds[i]
                                                   : op->spec.kinds[G_N_ELEMENTS(op->spec.kinds) - 1];

        if (!zg_value_valid(kind, value))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 的第 %u 个参数（%s）非法：%s",
                        op->id, i + 1, zg_value_kind_label(kind),
                        value != NULL ? value : "(空)");
            return FALSE;
        }
    }

    if (op->io != ZG_OP_IO_NONE)
    {
        if (zg_str_is_empty(request->io_path))
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 需要重定向文件路径（参数 %s）",
                        op->id, op->io_arg);
            return FALSE;
        }

        if (request->io_path[0] != '/')
        {
            g_set_error(error, ZG_JSON_ERROR, ZG_JSON_ERROR_USAGE,
                        "操作 %s 的重定向路径必须是绝对路径", op->id);
            return FALSE;
        }
    }

    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* argv 组装                                                                  */
/* ------------------------------------------------------------------------- */

char **
zg_op_build_argv(const ZgOpDef *op,
                 const ZgOpRequest *request,
                 const char *tool_path)
{
    GPtrArray *argv;
    guint i;

    g_return_val_if_fail(op != NULL, NULL);
    g_return_val_if_fail(tool_path != NULL, NULL);

    argv = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(argv, g_strdup(tool_path));

    if (!zg_str_is_empty(op->subcommand))
    {
        gchar **tokens = g_strsplit(op->subcommand, " ", -1);

        for (i = 0; tokens[i] != NULL; i++)
        {
            if (tokens[i][0] != '\0')
                g_ptr_array_add(argv, g_strdup(tokens[i]));
        }

        g_strfreev(tokens);
    }

    if (request != NULL)
    {
        for (i = 0; i < request->flags->len; i++)
            g_ptr_array_add(argv, g_strdup(g_ptr_array_index(request->flags, i)));

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);
            gchar *flag = g_strdup_printf("-%s", option->key);

            g_ptr_array_add(argv, flag);
            g_ptr_array_add(argv, g_strdup(option->value));
        }

        for (i = 0; i < request->positional->len; i++)
            g_ptr_array_add(argv, g_strdup(g_ptr_array_index(request->positional, i)));
    }

    g_ptr_array_add(argv, NULL);

    return (char **)g_ptr_array_free(argv, FALSE);
}

char *
zg_op_describe(const ZgOpDef *op,
               const ZgOpRequest *request)
{
    GString *text;
    guint i;

    g_return_val_if_fail(op != NULL, NULL);

    text = g_string_new(op->tool != NULL ? op->tool : op->id);

    if (!zg_str_is_empty(op->subcommand))
        g_string_append_printf(text, " %s", op->subcommand);

    if (request != NULL)
    {
        for (i = 0; i < request->flags->len; i++)
            g_string_append_printf(text, " %s", (char *)g_ptr_array_index(request->flags, i));

        for (i = 0; i < request->options->len; i++)
        {
            const ZgOpOption *option = g_ptr_array_index(request->options, i);

            g_string_append_printf(text, " -%s %s", option->key, option->value);
        }

        for (i = 0; i < request->positional->len; i++)
        {
            const char *value = g_ptr_array_index(request->positional, i);

            if (strchr(value, ' ') != NULL)
                g_string_append_printf(text, " '%s'", value);
            else
                g_string_append_printf(text, " %s", value);
        }
    }

    return g_string_free(text, FALSE);
}
