/* zg-app.h - 应用上下文（连接状态、系统能力、状态栏、任务日志）
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_APP_H
#define ZG_APP_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _ZgApp ZgApp;
typedef void (*ZgAppRefreshFunc)(gpointer data);

ZgApp *zg_app_new(void);
gint zg_app_run(ZgApp *app,
                int argc,
                char **argv);
ZgApp *zg_app_get_default(void);

GtkApplication *zg_app_get_application(ZgApp *app);
GtkWindow *zg_app_get_window(ZgApp *app);
void zg_app_set_window(ZgApp *app,
                       GtkWindow *window);

/* 页面注册刷新回调；点击「刷新」或操作完成后统一调用 */
typedef void (*ZgAppLiveFunc)(gpointer data);

/* 周期性的轻量刷新（容量、健康状态等），不会重置页面选择 */
void zg_app_register_live_refresh(ZgApp *app, ZgAppLiveFunc func, gpointer data);

void zg_app_register_refresh(ZgApp *app,
                             ZgAppRefreshFunc func,
                             gpointer data);
void zg_app_refresh_all(ZgApp *app);

/* 状态栏与任务日志 */
void zg_app_set_status(ZgApp *app,
                       const gchar *format,
                       ...) G_GNUC_PRINTF(2, 3);
void zg_app_log(ZgApp *app,
                const gchar *format,
                ...) G_GNUC_PRINTF(2, 3);
const gchar *zg_app_log_text(ZgApp *app);
void zg_app_set_log_buffer(ZgApp *app,
                           GtkTextBuffer *buffer);

/* 连接状态与能力 */
gboolean zg_app_is_connected(ZgApp *app);
gboolean zg_app_json_output(ZgApp *app);
void zg_app_reconnect(ZgApp *app);

const gchar *zg_app_daemon_version(ZgApp *app);
const gchar *zg_app_zfs_userland(ZgApp *app);
const gchar *zg_app_zfs_kernel(ZgApp *app);
const gchar *zg_app_hostname(ZgApp *app);
const gchar *zg_app_osrelease(ZgApp *app);
const gchar *zg_app_arch(ZgApp *app);
const gchar *zg_app_arc_size(ZgApp *app);
const gchar *zg_app_arc_max(ZgApp *app);
const gchar *zg_app_socket_path(ZgApp *app);

/* 危险操作的认证策略（"dangerous" / "all" / "none"） */
const gchar *zg_app_auth_mode(ZgApp *app);

/* 后台服务的 euid；0 表示具备特权 */
gint zg_app_daemon_euid(ZgApp *app);

/* 轻量级提示条（右下角 Toast） */
void zg_app_set_toast_widget(ZgApp *app,
                             GtkWidget *revealer,
                             GtkWidget *label);
void zg_app_toast(ZgApp *app,
                  const gchar *format,
                  ...) G_GNUC_PRINTF(2, 3);

/* 由 window 模块在构建界面后调用 */
void zg_app_set_status_widget(ZgApp *app,
                              GtkWidget *label);
void zg_app_set_banner_widget(ZgApp *app,
                              GtkWidget *revealer,
                              GtkWidget *banner);

G_END_DECLS

#endif /* ZG_APP_H */
