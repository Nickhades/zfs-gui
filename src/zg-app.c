/* zg-app.c - 应用上下文实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-app.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "zg-client.h"
#include "zg-json.h"
#include "zg-model.h"
#include "zg-ui.h"
#include "zg-util.h"
#include "zg-window.h"

typedef struct
{
    ZgAppRefreshFunc func;
    gpointer data;
} RefreshHook;

typedef struct
{
    ZgAppLiveFunc func;
    gpointer data;
} LiveHook;

struct _ZgApp
{
    GtkApplication *application;
    GtkWindow *window;
    GtkWidget *status_label;
    GtkWidget *banner_revealer;
    GtkWidget *banner_content;
    GPtrArray *refresh_hooks;
    GPtrArray *live_hooks;
    guint live_source;

    gboolean connected;
    gboolean json_output;
    gboolean probe_in_flight;
    guint retry_source;

    gchar *daemon_version;
    gchar *zfs_userland;
    gchar *zfs_kernel;
    gchar *hostname;
    gchar *osrelease;
    gchar *arch;
    gchar *arc_size;
    gchar *arc_max;
    gchar *socket_path;
    gchar *auth_mode;
    gint daemon_euid;

    GString *log;
    GtkTextBuffer *log_buffer;

    GtkWidget *toast_revealer;
    GtkWidget *toast_label;
    guint toast_source;
};

static ZgApp *zg_default_app = NULL;

/* ------------------------------------------------------------------------- */
/* 生命周期                                                                   */
/* ------------------------------------------------------------------------- */

ZgApp *
zg_app_get_default(void)
{
    return zg_default_app;
}

static void
replace_string(gchar **slot,
               const gchar *value)
{
    g_free(*slot);
    *slot = g_strdup(value);
}

static void
zg_app_on_activate(GtkApplication *application,
                   gpointer user_data)
{
    ZgApp *app = user_data;

    (void)application;

    if (app->window != NULL)
    {
        gtk_window_present(app->window);
        return;
    }

    zg_ui_init();

    app->window = GTK_WINDOW(zg_window_new(app));
    gtk_window_present(app->window);

    zg_app_log(app, "图形界面已启动，socket = %s", zg_client_socket_path());
    zg_app_reconnect(app);
}

ZgApp *
zg_app_new(void)
{
    ZgApp *app = g_new0(ZgApp, 1);

    app->log = g_string_new(NULL);
    app->refresh_hooks = g_ptr_array_new_with_free_func(g_free);
    app->live_hooks = g_ptr_array_new_with_free_func(g_free);
    app->socket_path = g_strdup(zg_client_socket_path());

    /* 允许通过环境变量改用独立应用 ID（便于同时运行测试实例） */
    app->application = gtk_application_new(
            g_getenv("ZFS_GUI_APP_ID") != NULL ? g_getenv("ZFS_GUI_APP_ID")
                                               : "org.freebsd.zfsgui",
            G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app->application, "activate",
                     G_CALLBACK(zg_app_on_activate), app);

    zg_default_app = app;

    return app;
}

gint zg_app_run(ZgApp *app,
                int argc,
                char **argv)
{
    gint status;

    g_return_val_if_fail(app != NULL, 1);

    status = g_application_run(G_APPLICATION(app->application), argc, argv);

    return status;
}

GtkApplication *
zg_app_get_application(ZgApp *app)
{
    return (app != NULL) ? app->application : NULL;
}

GtkWindow *
zg_app_get_window(ZgApp *app)
{
    return (app != NULL) ? app->window : NULL;
}

void zg_app_set_window(ZgApp *app,
                       GtkWindow *window)
{
    if (app != NULL)
        app->window = window;
}

/* ------------------------------------------------------------------------- */
/* 状态栏 / 日志                                                              */
/* ------------------------------------------------------------------------- */

void zg_app_set_status_widget(ZgApp *app,
                              GtkWidget *label)
{
    if (app != NULL)
        app->status_label = label;
}

void zg_app_set_banner_widget(ZgApp *app,
                              GtkWidget *revealer,
                              GtkWidget *banner)
{
    if (app == NULL)
        return;

    app->banner_revealer = revealer;
    app->banner_content = banner;
}

void zg_app_set_status(ZgApp *app,
                       const gchar *format,
                       ...)
{
    va_list args;
    gchar *message;

    if (app == NULL)
        return;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    if (app->status_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->status_label), message);

    g_free(message);
}

void zg_app_log(ZgApp *app,
                const gchar *format,
                ...)
{
    va_list args;
    gchar *message;
    struct tm tm;
    time_t now;
    gchar stamp[16];

    if (app == NULL)
        return;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    now = time(NULL);

    if (localtime_r(&now, &tm) != NULL &&
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm) > 0)
        g_string_append_printf(app->log, "[%s] %s\n", stamp, message);
    else
        g_string_append_printf(app->log, "%s\n", message);

    /* 限制日志长度，避免长时间运行占用过多内存 */
    if (app->log->len > 512 * 1024)
        g_string_erase(app->log, 0, app->log->len - 384 * 1024);

    if (app->log_buffer != NULL)
        gtk_text_buffer_set_text(app->log_buffer, app->log->str, -1);

    g_free(message);
}

const gchar *
zg_app_log_text(ZgApp *app)
{
    return (app != NULL && app->log != NULL) ? app->log->str : "";
}

void zg_app_set_log_buffer(ZgApp *app,
                           GtkTextBuffer *buffer)
{
    if (app == NULL)
        return;

    app->log_buffer = buffer;

    if (buffer != NULL)
        gtk_text_buffer_set_text(buffer, app->log->str, -1);
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

static gboolean
on_live_tick(gpointer data)
{
    ZgApp *app = data;
    guint i;

    if (!app->connected)
        return G_SOURCE_CONTINUE;

    for (i = 0; i < app->live_hooks->len; i++)
    {
        LiveHook *hook = g_ptr_array_index(app->live_hooks, i);

        if (hook->func != NULL)
            hook->func(hook->data);
    }

    return G_SOURCE_CONTINUE;
}

void zg_app_register_live_refresh(ZgApp *app,
                                  ZgAppLiveFunc func,
                                  gpointer data)
{
    LiveHook *hook;

    if (app == NULL || func == NULL)
        return;

    hook = g_new0(LiveHook, 1);
    hook->func = func;
    hook->data = data;
    g_ptr_array_add(app->live_hooks, hook);

    if (app->live_source == 0)
        app->live_source = g_timeout_add_seconds(10, on_live_tick, app);
}

void zg_app_register_refresh(ZgApp *app,
                             ZgAppRefreshFunc func,
                             gpointer data)
{
    RefreshHook *hook;

    if (app == NULL || func == NULL)
        return;

    hook = g_new0(RefreshHook, 1);
    hook->func = func;
    hook->data = data;

    g_ptr_array_add(app->refresh_hooks, hook);
}

void zg_app_refresh_all(ZgApp *app)
{
    guint i;

    if (app == NULL)
        return;

    if (!app->connected)
    {
        zg_app_reconnect(app);
        return;
    }

    for (i = 0; i < app->refresh_hooks->len; i++)
    {
        RefreshHook *hook = g_ptr_array_index(app->refresh_hooks, i);

        hook->func(hook->data);
    }

    zg_app_set_status(app, "已刷新（%s）", zg_client_socket_path());
}

/* ------------------------------------------------------------------------- */
/* 能力探测                                                                   */
/* ------------------------------------------------------------------------- */

static void
apply_system_info(ZgApp *app,
                  ZgClientResult *result)
{
    ZgJson *data = zg_client_result_data(result);

    if (data == NULL)
    {
        zg_app_log(app, "警告：system.info 返回的数据无法解析");
        return;
    }

    app->json_output = zg_json_object_get_bool(data, "json_output", FALSE);

    replace_string(&app->daemon_version,
                   zg_json_object_get_string(data, "daemon_version"));
    replace_string(&app->zfs_userland,
                   zg_json_object_get_string(data, "zfs_userland"));
    replace_string(&app->zfs_kernel,
                   zg_json_object_get_string(data, "zfs_kernel"));
    replace_string(&app->hostname,
                   zg_json_object_get_string(data, "hostname"));
    replace_string(&app->osrelease,
                   zg_json_object_get_string(data, "osrelease"));
    replace_string(&app->arch,
                   zg_json_object_get_string(data, "arch"));
    replace_string(&app->arc_size,
                   zg_json_object_get_string(data, "arc_size"));
    replace_string(&app->arc_max,
                   zg_json_object_get_string(data, "arc_max"));
    replace_string(&app->socket_path,
                   zg_json_object_get_string(data, "socket_path"));
    replace_string(&app->auth_mode,
                   zg_json_object_get_string(data, "auth_mode"));
    app->daemon_euid = (gint)zg_json_object_get_int(data, "euid", -1);

    zg_app_log(app, "已连接后台服务：%s（ZFS %s，JSON 输出%s）",
               zg_str_or(app->daemon_version, "?"),
               zg_str_or(app->zfs_userland, "未知"),
               app->json_output ? "可用" : "不可用");

    zg_json_free(data);
}

static gboolean retry_probe(gpointer data);

static void
on_probe_done(GObject *source,
              GAsyncResult *async_result,
              gpointer user_data)
{
    ZgApp *app = user_data;
    ZgClientResult *result = zg_client_call_finish(async_result);

    (void)source;

    app->probe_in_flight = FALSE;

    if (result == NULL)
    {
        app->connected = FALSE;
        zg_app_set_status(app, "后台服务无应答（将自动重试）");

        if (app->retry_source == 0)
            app->retry_source = g_timeout_add_seconds(3, retry_probe, app);

        return;
    }

    app->probe_in_flight = FALSE;

    if (result->ok)
    {
        app->connected = TRUE;

        if (app->retry_source != 0)
        {
            g_source_remove(app->retry_source);
            app->retry_source = 0;
        }

        apply_system_info(app, result);

        if (app->daemon_euid != 0 && app->banner_content != NULL)
        {
            zg_ui_banner_set(app->banner_content,
                             "后台服务没有特权",
                             "zfs-guid 当前以非 root 身份运行：管理员密码无法验证，"
                             "创建 / 修改 / 销毁等操作会失败。"
                             "请以 root 启动：service zfsguid start",
                             TRUE);

            if (app->banner_revealer != NULL)
                gtk_revealer_set_reveal_child(GTK_REVEALER(app->banner_revealer), TRUE);
        }
        else if (app->banner_revealer != NULL)
        {
            gtk_revealer_set_reveal_child(GTK_REVEALER(app->banner_revealer), FALSE);
        }

        zg_app_set_status(app, "已连接 %s（%s）",
                          zg_str_or(app->hostname, "本机"),
                          zg_str_or(app->zfs_userland, "ZFS"));
    }
    else
    {
        app->connected = FALSE;

        if (app->banner_content != NULL)
        {
            zg_ui_banner_set(app->banner_content,
                             "未连接后台服务",
                             "请先启动后台服务：service zfsguid onestart"
                             "（配置见 /usr/local/etc/zfs-gui.conf）",
                             TRUE);

            if (app->banner_revealer != NULL)
                gtk_revealer_set_reveal_child(GTK_REVEALER(app->banner_revealer), TRUE);

            {
                GtkWidget *retry = gtk_button_new_with_label("立即重连");

                gtk_widget_add_css_class(retry, "suggested-action");
                gtk_widget_set_valign(retry, GTK_ALIGN_CENTER);
                g_signal_connect_swapped(retry, "clicked",
                                         G_CALLBACK(zg_app_reconnect), app);
                gtk_box_append(GTK_BOX(app->banner_content), retry);
            }
        }

        zg_app_set_status(app, "未连接后台服务：请执行 service zfsguid onestart（将自动重试）");
        zg_app_log(app, "连接失败：%s", result->message);

        if (app->retry_source == 0)
            app->retry_source = g_timeout_add_seconds(3, retry_probe, app);
    }

    zg_client_result_free(result);

    if (app->connected)
        zg_app_refresh_all(app);
}

static gboolean
retry_probe(gpointer data)
{
    ZgApp *app = data;

    app->retry_source = 0;

    if (!app->connected)
    {
        if (app->probe_in_flight)
        {
            /* 上一次探测迟迟没有回调：保持重试链存活 */
            app->retry_source = g_timeout_add_seconds(3, retry_probe, app);
        }
        else
        {
            zg_app_reconnect(app);
        }
    }

    return G_SOURCE_REMOVE;
}

void zg_app_reconnect(ZgApp *app)
{
    if (app == NULL)
        return;

    zg_app_set_status(app, "正在连接后台服务 %s …", zg_client_socket_path());

    if (app->probe_in_flight)
    {
        if (app->retry_source == 0)
            app->retry_source = g_timeout_add_seconds(3, retry_probe, app);
        return;
    }

    app->probe_in_flight = TRUE;
    zg_client_call_async("system.info", NULL, on_probe_done, app);
}

/* ------------------------------------------------------------------------- */
/* 访问器                                                                     */
/* ------------------------------------------------------------------------- */

gboolean
zg_app_is_connected(ZgApp *app)
{
    return app != NULL && app->connected;
}

gboolean
zg_app_json_output(ZgApp *app)
{
    return app != NULL && app->json_output;
}

const gchar *zg_app_daemon_version(ZgApp *app)
{
    return app ? app->daemon_version : NULL;
}
const gchar *zg_app_zfs_userland(ZgApp *app)
{
    return app ? app->zfs_userland : NULL;
}
const gchar *zg_app_zfs_kernel(ZgApp *app)
{
    return app ? app->zfs_kernel : NULL;
}
const gchar *zg_app_hostname(ZgApp *app)
{
    return app ? app->hostname : NULL;
}
const gchar *zg_app_osrelease(ZgApp *app)
{
    return app ? app->osrelease : NULL;
}
const gchar *zg_app_arch(ZgApp *app)
{
    return app ? app->arch : NULL;
}
const gchar *zg_app_arc_size(ZgApp *app)
{
    return app ? app->arc_size : NULL;
}
const gchar *zg_app_arc_max(ZgApp *app)
{
    return app ? app->arc_max : NULL;
}
const gchar *zg_app_socket_path(ZgApp *app)
{
    return app ? app->socket_path : NULL;
}

gint zg_app_daemon_euid(ZgApp *app)
{
    return app != NULL ? app->daemon_euid : -1;
}

const gchar *
zg_app_auth_mode(ZgApp *app)
{
    if (app == NULL || zg_str_is_empty(app->auth_mode))
        return "dangerous";

    return app->auth_mode;
}

/* ------------------------------------------------------------------------- */
/* Toast                                                                      */
/* ------------------------------------------------------------------------- */

static gboolean
toast_timeout(gpointer data)
{
    ZgApp *app = data;

    app->toast_source = 0;

    if (app->toast_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(app->toast_revealer), FALSE);

    return G_SOURCE_REMOVE;
}

void zg_app_set_toast_widget(ZgApp *app,
                             GtkWidget *revealer,
                             GtkWidget *label)
{
    if (app == NULL)
        return;

    app->toast_revealer = revealer;
    app->toast_label = label;
}

void zg_app_toast(ZgApp *app,
                  const gchar *format,
                  ...)
{
    va_list args;
    gchar *message;

    if (app == NULL)
        return;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    if (app->toast_label != NULL)
        gtk_label_set_text(GTK_LABEL(app->toast_label), message);

    if (app->toast_revealer != NULL)
        gtk_revealer_set_reveal_child(GTK_REVEALER(app->toast_revealer), TRUE);

    if (app->toast_source != 0)
        g_source_remove(app->toast_source);

    app->toast_source = g_timeout_add(3200, toast_timeout, app);

    g_free(message);
}
