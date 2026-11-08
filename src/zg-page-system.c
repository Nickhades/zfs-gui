/* zg-page-system.c - 系统信息与 ARC 统计页面
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-model.h"
#include "zg-pages.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GtkWidget *ov_host;
    GtkWidget *ov_os;
    GtkWidget *ov_arch;
    GtkWidget *ov_daemon;
    GtkWidget *ov_userland;
    GtkWidget *ov_kernel;
    GtkWidget *ov_json;
    GtkWidget *ov_socket;
    GtkWidget *ov_pools;
    GtkWidget *arc_summary;
    GtkTextBuffer *arc_buffer;
    GtkTextBuffer *log_buffer;
} SystemPage;

/* ------------------------------------------------------------------------- */
/* 系统信息                                                                   */
/* ------------------------------------------------------------------------- */

static void
system_update_info(SystemPage *page)
{
    ZgApp *app = page->app;

    gtk_label_set_text(GTK_LABEL(page->ov_host),
                       zg_str_or(zg_app_hostname(app), "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_os),
                       zg_str_or(zg_app_osrelease(app), "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_arch),
                       zg_str_or(zg_app_arch(app), "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_daemon),
                       zg_str_or(zg_app_daemon_version(app), "未连接"));
    gtk_label_set_text(GTK_LABEL(page->ov_userland),
                       zg_str_or(zg_app_zfs_userland(app), "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_kernel),
                       zg_str_or(zg_app_zfs_kernel(app), "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_json),
                       zg_app_json_output(app) ? "支持（OpenZFS ≥ 2.2）" : "不支持（使用兼容解析）");
    gtk_label_set_text(GTK_LABEL(page->ov_socket),
                       zg_str_or(zg_app_socket_path(app), zg_client_socket_path()));
}

/* ------------------------------------------------------------------------- */
/* ARC 统计                                                                   */
/* ------------------------------------------------------------------------- */

static gboolean
arc_lookup(GPtrArray *stats,
           const gchar *name,
           const gchar **value_out)
{
    guint i;

    for (i = 0; i < stats->len; i++)
    {
        ZgProperty *property = g_ptr_array_index(stats, i);

        if (g_strcmp0(property->property, name) == 0)
        {
            *value_out = property->value;
            return TRUE;
        }
    }

    return FALSE;
}

static void
on_arcstats(GObject *source,
            GAsyncResult *result,
            gpointer user_data)
{
    SystemPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GPtrArray *stats;
    GString *text;
    guint i;

    (void)source;

    if (response == NULL || !response->ok)
    {
        gtk_label_set_text(GTK_LABEL(page->arc_summary),
                           response != NULL ? response->message : "读取 ARC 统计失败");
        zg_client_result_free(response);
        return;
    }

    stats = zg_model_arcstats_from_text(response->stdout_text);

    text = g_string_new(NULL);

    for (i = 0; i < stats->len; i++)
    {
        ZgProperty *property = g_ptr_array_index(stats, i);
        const gchar *short_name = property->property;

        if (g_str_has_prefix(short_name, "kstat.zfs.misc.arcstats."))
            short_name += strlen("kstat.zfs.misc.arcstats.");

        g_string_append_printf(text, "%-32s %s\n", short_name, property->value);
    }

    zg_ui_text_set(page->arc_buffer, text->str);
    g_string_free(text, TRUE);

    {
        const gchar *hits_text = NULL;
        const gchar *misses_text = NULL;
        const gchar *size_text = NULL;
        const gchar *max_text = NULL;
        guint64 hits = 0;
        guint64 misses = 0;

        if (arc_lookup(stats, "kstat.zfs.misc.arcstats.hits", &hits_text))
            zg_parse_u64(hits_text, &hits);

        if (arc_lookup(stats, "kstat.zfs.misc.arcstats.misses", &misses_text))
            zg_parse_u64(misses_text, &misses);

        arc_lookup(stats, "kstat.zfs.misc.arcstats.size", &size_text);
        arc_lookup(stats, "kstat.zfs.misc.arcstats.c_max", &max_text);

        {
            gchar *summary;
            gchar *size_formatted = NULL;
            gchar *max_formatted = NULL;

            if (size_text != NULL)
            {
                guint64 bytes = 0;

                zg_parse_u64(size_text, &bytes);
                size_formatted = zg_format_bytes(bytes);
            }

            if (max_text != NULL)
            {
                guint64 bytes = 0;

                zg_parse_u64(max_text, &bytes);
                max_formatted = zg_format_bytes(bytes);
            }

            if (hits + misses > 0)
            {
                summary = g_strdup_printf(
                        "缓存命中率 %.2f%%（命中 %" G_GUINT64_FORMAT " 次 / 未命中 %" G_GUINT64_FORMAT " 次）"
                        " · 当前大小 %s · 上限 %s",
                        (double)hits * 100.0 / (double)(hits + misses),
                        hits, misses,
                        size_formatted != NULL ? size_formatted : "—",
                        max_formatted != NULL ? max_formatted : "—");
            }
            else
            {
                summary = g_strdup_printf(
                        "命中率暂无数据 · 当前大小 %s · 上限 %s",
                        size_formatted != NULL ? size_formatted : "—",
                        max_formatted != NULL ? max_formatted : "—");
            }

            gtk_label_set_text(GTK_LABEL(page->arc_summary), summary);

            g_free(summary);
            g_free(size_formatted);
            g_free(max_formatted);
        }
    }

    g_ptr_array_free(stats, TRUE);
    zg_client_result_free(response);
}

static void
system_refresh_arc(SystemPage *page)
{
    if (!zg_app_is_connected(page->app))
        return;

    zg_app_set_status(page->app, "正在读取 ARC 统计…");
    zg_client_call_async("system.arcstats", NULL, on_arcstats, page);
}

static void
system_refresh(gpointer data)
{
    SystemPage *page = data;

    system_update_info(page);
    system_refresh_arc(page);
}

/* ------------------------------------------------------------------------- */
/* 按钮回调                                                                   */
/* ------------------------------------------------------------------------- */

static void
on_refresh_info(GtkButton *button,
                gpointer user_data)
{
    SystemPage *page = user_data;

    (void)button;

    if (!zg_app_is_connected(page->app))
    {
        zg_app_reconnect(page->app);
        return;
    }

    system_update_info(page);
    zg_app_set_status(page->app, "系统信息已更新");
}

static void
on_refresh_arc(GtkButton *button,
               gpointer user_data)
{
    (void)button;

    system_refresh_arc(user_data);
}

static void
on_clear_log(GtkButton *button,
             gpointer user_data)
{
    (void)button;
    (void)user_data;

    /* 任务日志由 ZgApp 持有，这里仅提示用户日志会持续累积 */
    zg_app_set_status(zg_app_get_default(), "任务日志会保留最近的运行记录");
}

GtkWidget *
zg_page_system_new(ZgApp *app)
{
    SystemPage *page = g_new0(SystemPage, 1);
    GtkWidget *scrolled;
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *row_box;
    GtkWidget *arc_view;
    GtkWidget *log_view;
    gint row = 0;

    page->app = app;

    scrolled = gtk_scrolled_window_new();
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    gtk_box_append(GTK_BOX(content), zg_ui_label_heading("系统与 ZFS 版本"));

    grid = zg_ui_grid_new();
    page->ov_host = zg_ui_grid_add(grid, &row, "主机名", "—");
    page->ov_os = zg_ui_grid_add(grid, &row, "系统版本", "—");
    page->ov_arch = zg_ui_grid_add(grid, &row, "架构", "—");
    page->ov_daemon = zg_ui_grid_add(grid, &row, "后台服务", "—");
    page->ov_userland = zg_ui_grid_add(grid, &row, "ZFS 用户态", "—");
    page->ov_kernel = zg_ui_grid_add(grid, &row, "ZFS 内核", "—");
    page->ov_json = zg_ui_grid_add(grid, &row, "JSON 输出", "—");
    page->ov_socket = zg_ui_grid_add(grid, &row, "socket 路径", "—");
    page->ov_pools = zg_ui_grid_add(grid, &row, "能力说明",
                                    "列表读取使用 -H -p（全版本兼容）；状态树在支持时使用 -j，否则解析文本输出。");

    gtk_label_set_wrap(GTK_LABEL(page->ov_pools), TRUE);
    gtk_label_set_wrap(GTK_LABEL(page->ov_socket), TRUE);

    gtk_box_append(GTK_BOX(content), grid);

    row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row_box),
                   zg_ui_button("刷新系统信息", G_CALLBACK(on_refresh_info), page));
    gtk_box_append(GTK_BOX(content), row_box);

    gtk_box_append(GTK_BOX(content), zg_ui_label_heading("ARC 缓存统计"));

    page->arc_summary = zg_ui_label_wrap("尚未读取。");
    gtk_box_append(GTK_BOX(content), page->arc_summary);

    arc_view = zg_ui_text_view_new(&page->arc_buffer, FALSE);
    gtk_widget_set_size_request(arc_view, -1, 220);
    gtk_box_append(GTK_BOX(content), arc_view);

    row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row_box),
                   zg_ui_button("刷新 ARC", G_CALLBACK(on_refresh_arc), page));
    gtk_box_append(GTK_BOX(content), row_box);

    gtk_box_append(GTK_BOX(content), zg_ui_label_heading("任务日志"));
    gtk_box_append(GTK_BOX(content), zg_ui_label_dim(
                                             "记录本界面发起的每一次命令及其结果，便于事后核对。"));

    log_view = zg_ui_text_view_new(&page->log_buffer, TRUE);
    gtk_widget_set_size_request(log_view, -1, 200);
    gtk_box_append(GTK_BOX(content), log_view);

    row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row_box, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row_box),
                   zg_ui_button("提示日志位置", G_CALLBACK(on_clear_log), page));
    gtk_box_append(GTK_BOX(content), row_box);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);

    page->root = scrolled;

    zg_app_set_log_buffer(app, page->log_buffer);

    g_object_set_data_full(G_OBJECT(page->root), "zg-page-system", page, g_free);

    zg_app_register_refresh(app, system_refresh, page);

    return page->root;
}
