/* zg-client.h - 前端与守护进程通信的客户端
 *
 * 请求在后台线程中同步完成，通过 GTask 回到主线程回调，
 * 因此界面不会因长耗时 ZFS 命令而冻结。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_CLIENT_H
#define ZG_CLIENT_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
    gboolean ok;
    gint code;
    gchar *stdout_text;
    gchar *stderr_text;
    gchar *data_json;
    gchar *message;
} ZgClientResult;

/* ------------------------------------------------------------------------- */
/* 参数构造器                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct _ZgArgs ZgArgs;

ZgArgs *zg_args_new(void);
void zg_args_free(ZgArgs *args);
void zg_args_add_flag(ZgArgs *args,
                      const gchar *flag);
void zg_args_add_option(ZgArgs *args,
                        const gchar *key,
                        const gchar *value);
void zg_args_add_positional(ZgArgs *args,
                            const gchar *value);
void zg_args_add_positional_int(ZgArgs *args,
                                gint64 value);
void zg_args_set_io(ZgArgs *args,
                    const gchar *key,
                    const gchar *path);
gboolean zg_args_is_empty(ZgArgs *args);

/* 生成 args JSON 对象文本并释放 args */
gchar *zg_args_to_json(ZgArgs *args);

/* ------------------------------------------------------------------------- */
/* 调用                                                                       */
/* ------------------------------------------------------------------------- */

/* 同步调用（在工作线程中使用）。
 * 连接失败时同样返回结果对象（ok == FALSE），只有内存等致命错误返回 NULL。 */
ZgClientResult *zg_client_call_sync(const gchar *op,
                                    const gchar *args_json,
                                    GError **error);

/* 异步调用；args_json 可为 NULL */
void zg_client_call_async(const gchar *op,
                          gchar *args_json,
                          GAsyncReadyCallback callback,
                          gpointer user_data);

/* 带管理员凭据的异步调用（用于危险操作的二次认证）。
 * username/password 会被复制到请求中，password 在请求发送后清零。 */
void zg_client_call_async_auth(const gchar *op,
                               gchar *args_json,
                               const gchar *username,
                               const gchar *password,
                               GAsyncReadyCallback callback,
                               gpointer user_data);

ZgClientResult *zg_client_call_finish(GAsyncResult *result);

void zg_client_result_free(ZgClientResult *result);

/* 解析 result->data_json（调用方负责 zg_json_free） */
struct _ZgJson;
struct _ZgJson *zg_client_result_data(const ZgClientResult *result);

/* 当前生效的 socket 路径（支持 ZFS_GUI_SOCKET 环境变量，便于开发调试） */
const gchar *zg_client_socket_path(void);

G_END_DECLS

#endif /* ZG_CLIENT_H */
