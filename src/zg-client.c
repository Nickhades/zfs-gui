/* zg-client.c - 前端与守护进程通信实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-client.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "zg-io.h"
#include "zg-json.h"
#include "zg-protocol.h"
#include "zg-util.h"

/* ------------------------------------------------------------------------- */
/* 结果对象                                                                   */
/* ------------------------------------------------------------------------- */

void zg_client_result_free(ZgClientResult *result)
{
    if (result == NULL)
        return;

    g_free(result->stdout_text);
    g_free(result->stderr_text);
    g_free(result->data_json);
    g_free(result->message);
    g_free(result);
}

static ZgClientResult *
result_new(void)
{
    ZgClientResult *result = g_new0(ZgClientResult, 1);

    result->stdout_text = g_strdup("");
    result->stderr_text = g_strdup("");
    result->message = g_strdup("");

    return result;
}

ZgJson *
zg_client_result_data(const ZgClientResult *result)
{
    if (result == NULL || zg_str_is_empty(result->data_json))
        return NULL;

    return zg_json_parse(result->data_json, -1, NULL);
}

/* ZFS 的用户属性等字段理论上可能包含非 UTF-8 字节，
 * 统一在客户端边界清洗，避免整个界面出现 Pango 警告。 */
static void
sanitize_utf8(gchar **text)
{
    gchar *valid;

    if (text == NULL || *text == NULL)
        return;

    if (g_utf8_validate(*text, -1, NULL))
        return;

    valid = g_utf8_make_valid(*text, -1);
    g_free(*text);
    *text = valid;
}

static ZgClientResult *
connection_failure(const gchar *message)
{
    ZgClientResult *result = result_new();

    result->ok = FALSE;
    result->code = -1;
    g_free(result->message);
    result->message = g_strdup(message);

    return result;
}

/* ------------------------------------------------------------------------- */
/* 参数构造器                                                                 */
/* ------------------------------------------------------------------------- */

struct _ZgArgs
{
    GPtrArray *flags;
    GPtrArray *positional;
    GPtrArray *option_keys;
    GPtrArray *option_values;
    gchar *io_key;
    gchar *io_path;
};

ZgArgs *
zg_args_new(void)
{
    ZgArgs *args = g_new0(ZgArgs, 1);

    args->flags = g_ptr_array_new_with_free_func(g_free);
    args->positional = g_ptr_array_new_with_free_func(g_free);
    args->option_keys = g_ptr_array_new_with_free_func(g_free);
    args->option_values = g_ptr_array_new_with_free_func(g_free);

    return args;
}

void zg_args_free(ZgArgs *args)
{
    if (args == NULL)
        return;

    g_ptr_array_free(args->flags, TRUE);
    g_ptr_array_free(args->positional, TRUE);
    g_ptr_array_free(args->option_keys, TRUE);
    g_ptr_array_free(args->option_values, TRUE);
    g_free(args->io_key);
    g_free(args->io_path);
    g_free(args);
}

void zg_args_add_flag(ZgArgs *args,
                      const gchar *flag)
{
    g_return_if_fail(args != NULL);
    g_return_if_fail(flag != NULL && flag[0] == '-');

    g_ptr_array_add(args->flags, g_strdup(flag));
}

void zg_args_add_option(ZgArgs *args,
                        const gchar *key,
                        const gchar *value)
{
    g_return_if_fail(args != NULL);
    g_return_if_fail(key != NULL);
    g_return_if_fail(value != NULL);

    g_ptr_array_add(args->option_keys, g_strdup(key));
    g_ptr_array_add(args->option_values, g_strdup(value));
}

void zg_args_add_positional(ZgArgs *args,
                            const gchar *value)
{
    g_return_if_fail(args != NULL);
    g_return_if_fail(value != NULL);

    g_ptr_array_add(args->positional, g_strdup(value));
}

void zg_args_add_positional_int(ZgArgs *args,
                                gint64 value)
{
    gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, value);

    zg_args_add_positional(args, text);
    g_free(text);
}

void zg_args_set_io(ZgArgs *args,
                    const gchar *key,
                    const gchar *path)
{
    g_return_if_fail(args != NULL);

    g_free(args->io_key);
    g_free(args->io_path);

    args->io_key = g_strdup(key);
    args->io_path = g_strdup(path);
}

gboolean
zg_args_is_empty(ZgArgs *args)
{
    return args == NULL ||
           (args->flags->len == 0 &&
            args->positional->len == 0 &&
            args->option_keys->len == 0 &&
            args->io_path == NULL);
}

gchar *
zg_args_to_json(ZgArgs *args)
{
    ZgJsonWriter *writer;
    gchar *text;
    guint i;

    if (args == NULL)
        return g_strdup("{}");

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);

    if (args->flags->len > 0)
    {
        zg_json_writer_begin_array(writer, ZG_KEY_FLAGS);

        for (i = 0; i < args->flags->len; i++)
            zg_json_writer_add_string(writer, g_ptr_array_index(args->flags, i));

        zg_json_writer_end_array(writer);
    }

    if (args->positional->len > 0)
    {
        zg_json_writer_begin_array(writer, ZG_KEY_POSITIONAL);

        for (i = 0; i < args->positional->len; i++)
            zg_json_writer_add_string(writer, g_ptr_array_index(args->positional, i));

        zg_json_writer_end_array(writer);
    }

    if (args->option_keys->len > 0)
    {
        zg_json_writer_begin_array(writer, ZG_KEY_OPTIONS);

        for (i = 0; i < args->option_keys->len; i++)
        {
            zg_json_writer_begin_object(writer, NULL);
            zg_json_writer_set_string(writer, ZG_KEY_KEY,
                                      g_ptr_array_index(args->option_keys, i));
            zg_json_writer_set_string(writer, ZG_KEY_VALUE,
                                      g_ptr_array_index(args->option_values, i));
            zg_json_writer_end_object(writer);
        }

        zg_json_writer_end_array(writer);
    }

    if (args->io_path != NULL)
        zg_json_writer_set_string(writer, args->io_key, args->io_path);

    zg_json_writer_end_object(writer);

    text = zg_json_writer_to_string(writer, NULL);

    zg_args_free(args);

    return text;
}

/* ------------------------------------------------------------------------- */
/* socket 路径                                                                */
/* ------------------------------------------------------------------------- */

const gchar *
zg_client_socket_path(void)
{
    static gchar *cached = NULL;

    if (cached == NULL)
    {
        const gchar *override = g_getenv("ZFS_GUI_SOCKET");

        cached = g_strdup(zg_str_is_empty(override) ? ZG_SOCKET_PATH_DEFAULT
                                                    : override);
    }

    return cached;
}

/* ------------------------------------------------------------------------- */
/* 同步调用                                                                   */
/* ------------------------------------------------------------------------- */

static gint
connect_to_daemon(GError **error)
{
    const gchar *path = zg_client_socket_path();
    struct sockaddr_un address;
    gint fd;

    if (strlen(path) >= sizeof(address.sun_path))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "socket 路径过长");
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
    {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "创建 socket 失败：%s", g_strerror(errno));
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    g_strlcpy(address.sun_path, path, sizeof(address.sun_path));

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                    "无法连接后台服务 %s：%s%s", path, g_strerror(errno),
                    (errno == EACCES || errno == EPERM)
                            ? "（权限不足：请确认当前用户在 wheel 组，"
                              "或已加入 allowed_users；目录需可进入）"
                            : "");
        close(fd);
        return -1;
    }

    /* 长耗时操作（send/receive/scrub -w）没有上限，仅设置发送超时 */
    zg_io_set_timeouts(fd, 0, 60);

    return fd;
}

static ZgClientResult *
perform_call(const gchar *op,
             const gchar *args_json,
             const gchar *auth_user,
             const gchar *auth_password,
             GError **error)
{
    ZgJsonWriter *writer;
    gchar *request;
    gsize request_length = 0;
    gchar *response = NULL;
    gsize response_length = 0;
    gint fd;
    ZgClientResult *result;
    ZgJson *root = NULL;
    GError *local_error = NULL;

    fd = connect_to_daemon(&local_error);

    if (fd < 0)
    {
        gchar *message = g_strdup_printf(
                "%s\n\n请确认后台服务已启动：\n"
                "    service zfsguid onestart\n\n"
                "（首次使用请参考安装说明配置 " ZG_CONFIG_PATH_DEFAULT "）",
                local_error != NULL ? local_error->message : "无法连接后台服务");
        ZgClientResult *failure = connection_failure(message);

        g_free(message);
        g_clear_error(&local_error);

        if (error != NULL)
            *error = NULL;

        return failure;
    }

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_int(writer, ZG_KEY_VERSION, ZG_PROTOCOL_VERSION);
    zg_json_writer_set_string(writer, ZG_KEY_OP, op);
    zg_json_writer_set_raw(writer, ZG_KEY_ARGS,
                           (args_json != NULL && args_json[0] != '\0')
                                   ? args_json
                                   : "{}");

    if (auth_user != NULL && auth_user[0] != '\0')
    {
        zg_json_writer_begin_object(writer, ZG_KEY_AUTH);
        zg_json_writer_set_string(writer, ZG_KEY_AUTH_USER, auth_user);
        zg_json_writer_set_string(writer, ZG_KEY_AUTH_PASS,
                                  auth_password != NULL ? auth_password : "");
        zg_json_writer_end_object(writer);
    }

    zg_json_writer_end_object(writer);

    request = zg_json_writer_to_string(writer, &request_length);

    if (!zg_io_write_frame(fd, request, request_length, &local_error))
    {
        g_free(request);
        close(fd);

        result = connection_failure(local_error != NULL ? local_error->message
                                                        : "发送请求失败");
        g_clear_error(&local_error);

        if (error != NULL)
            *error = NULL;

        return result;
    }

    g_free(request);

    response = zg_io_read_frame(fd, &response_length, &local_error);
    close(fd);

    if (response == NULL)
    {
        result = connection_failure(local_error != NULL ? local_error->message
                                                        : "读取应答失败");
        g_clear_error(&local_error);

        if (error != NULL)
            *error = NULL;

        return result;
    }

    root = zg_json_parse(response, (gssize)response_length, &local_error);

    if (root == NULL)
    {
        gchar *message = g_strdup_printf("应答不是合法 JSON：%s",
                                         local_error != NULL ? local_error->message : "未知错误");

        result = connection_failure(message);
        g_free(message);
        g_clear_error(&local_error);
        g_free(response);

        if (error != NULL)
            *error = NULL;

        return result;
    }

    result = result_new();
    result->ok = zg_json_object_get_bool(root, ZG_KEY_OK, FALSE);
    result->code = (gint)zg_json_object_get_int(root, ZG_KEY_CODE, -1);

    g_free(result->stdout_text);
    result->stdout_text = g_strdup(zg_json_object_get_string_or(root, ZG_KEY_STDOUT, ""));

    g_free(result->stderr_text);
    result->stderr_text = g_strdup(zg_json_object_get_string_or(root, ZG_KEY_STDERR, ""));

    g_free(result->message);
    result->message = g_strdup(zg_json_object_get_string_or(root, ZG_KEY_MESSAGE, ""));

    {
        const ZgJson *data = zg_json_object_get(root, ZG_KEY_DATA);

        if (data != NULL && zg_json_get_type(data) == ZG_JSON_TYPE_OBJECT)
            result->data_json = zg_json_to_text(data);
    }

    sanitize_utf8(&result->stdout_text);
    sanitize_utf8(&result->stderr_text);
    sanitize_utf8(&result->message);

    zg_json_free(root);
    g_free(response);

    if (error != NULL)
        *error = NULL;

    return result;
}

ZgClientResult *
zg_client_call_sync(const gchar *op,
                    const gchar *args_json,
                    GError **error)
{
    g_return_val_if_fail(op != NULL, NULL);

    return perform_call(op, args_json, NULL, NULL, error);
}

/* ------------------------------------------------------------------------- */
/* 异步调用                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    gchar *op;
    gchar *args_json;
    gchar *auth_user;
    gchar *auth_password;
} CallRequest;

static void
call_request_free(gpointer data)
{
    CallRequest *request = data;

    if (request == NULL)
        return;

    g_free(request->op);
    g_free(request->args_json);
    g_free(request->auth_user);

    if (request->auth_password != NULL)
    {
        explicit_bzero(request->auth_password, strlen(request->auth_password));
        g_free(request->auth_password);
    }

    g_free(request);
}

static void
call_thread(GTask *task,
            gpointer source_object,
            gpointer task_data,
            GCancellable *cancellable)
{
    CallRequest *request = task_data;
    ZgClientResult *result;

    (void)source_object;
    (void)cancellable;

    result = perform_call(request->op, request->args_json,
                          request->auth_user, request->auth_password, NULL);

    /* 凭据只用于本次请求，用完立即清除 */
    if (request->auth_password != NULL)
    {
        explicit_bzero(request->auth_password, strlen(request->auth_password));
        g_free(request->auth_password);
        request->auth_password = NULL;
    }

    if (result == NULL)
    {
        ZgClientResult *failure = result_new();

        failure->ok = FALSE;
        failure->code = -1;
        g_free(failure->message);
        failure->message = g_strdup("内存不足，请求未能完成");
        result = failure;
    }

    g_task_return_pointer(task, result, (GDestroyNotify)zg_client_result_free);
}

void zg_client_call_async_auth(const gchar *op,
                               gchar *args_json,
                               const gchar *username,
                               const gchar *password,
                               GAsyncReadyCallback callback,
                               gpointer user_data)
{
    GTask *task;
    CallRequest *request;

    g_return_if_fail(op != NULL);

    task = g_task_new(NULL, NULL, callback, user_data);
    g_task_set_source_tag(task, (gpointer)zg_client_call_async_auth);

    request = g_new0(CallRequest, 1);
    request->op = g_strdup(op);
    request->args_json = (args_json != NULL) ? args_json : g_strdup("{}");
    request->auth_user = g_strdup(username);
    request->auth_password = g_strdup(password);

    g_task_set_task_data(task, request, call_request_free);
    g_task_run_in_thread(task, call_thread);

    g_object_unref(task);
}

void zg_client_call_async(const gchar *op,
                          gchar *args_json,
                          GAsyncReadyCallback callback,
                          gpointer user_data)
{
    zg_client_call_async_auth(op, args_json, NULL, NULL, callback, user_data);
}

ZgClientResult *
zg_client_call_finish(GAsyncResult *result)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);

    return g_task_propagate_pointer(G_TASK(result), NULL);
}
