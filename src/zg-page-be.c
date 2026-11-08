/* zg-page-be.c - 启动环境（Boot Environment）页面
 *
 * 基于 FreeBSD 的 bectl(8)：列出、创建、激活、重命名、销毁、挂载启动环境。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-actions.h"
#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-pages.h"
#include "zg-table.h"
#include "zg-ui.h"
#include "zg-util.h"

enum
{
    BE_COL_NAME,
    BE_COL_ACTIVE,
    BE_COL_MOUNT,
    BE_COL_SPACE,
    BE_COL_CREATED,
    BE_N_COLUMNS
};

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GListStore *store;
    GtkWidget *table;
    GtkWidget *panel;
    gchar *selected;
} BePage;

static void be_update_panel(BePage *page);
static void on_table_activated(GtkColumnView *view, guint position, gpointer user_data);
static void on_be_activate_clicked(GtkButton *button, gpointer user_data);

static GtkWindow *
page_window(BePage *page)
{
    return GTK_WINDOW(gtk_widget_get_root(page->root));
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_be_list(GObject *source,
           GAsyncResult *result,
           gpointer user_data)
{
    BePage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    gchar **lines;
    guint i;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_app_set_status(page->app, "读取启动环境失败：%s",
                          response != NULL ? response->message : "无应答");
        goto out;
    }

    g_list_store_remove_all(page->store);

    lines = g_strsplit(response->stdout_text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar **cols;
        guint count;

        if (lines[i][0] == '\0')
            continue;

        cols = g_strsplit(lines[i], "\t", -1);
        count = g_strv_length(cols);

        if (count < 2 || strchr(cols[0], '@') != NULL)
        {
            g_strfreev(cols);
            continue;
        }

        {
            ZgRow *row = zg_row_new();

            zg_row_set_text(row, BE_COL_NAME, cols[0]);
            zg_row_set_text(row, BE_COL_ACTIVE, count > 1 ? cols[1] : "");
            zg_row_set_text(row, BE_COL_MOUNT, count > 2 ? cols[2] : "");
            zg_row_set_text(row, BE_COL_SPACE, count > 3 ? cols[3] : "");
            zg_row_set_text(row, BE_COL_CREATED, count > 4 ? cols[4] : "");

            g_list_store_append(page->store, row);
            g_object_unref(row);
        }

        g_strfreev(cols);
    }

    g_strfreev(lines);

    zg_app_set_status(page->app, "启动环境列表已更新");

    zg_table_select_first(page->table);

out:
    zg_client_result_free(response);
}

static void
be_refresh(gpointer data)
{
    BePage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    /* 只列启动环境本身：-s 会把 BE 快照（name@snap）一并列出，
     * 那些条目不能激活/挂载，属于「快照」页的管理范围。 */
    args = zg_args_new();
    zg_args_add_flag(args, "-H");

    zg_client_call_async("bectl.list", zg_args_to_json(args), on_be_list, page);
}

/* ------------------------------------------------------------------------- */
/* 操作                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_be_selection_changed(GtkSelectionModel *selection,
                        guint position,
                        guint n_items,
                        gpointer user_data)
{
    BePage *page = user_data;
    ZgRow *row;
    const gchar *name;

    (void)selection;
    (void)position;
    (void)n_items;

    row = zg_table_get_selected(page->table);

    if (row == NULL)
        return;

    name = zg_row_get_text(row, BE_COL_NAME);

    if (name == NULL || name[0] == '\0')
        return;

    g_free(page->selected);
    page->selected = g_strdup(name);
    zg_app_set_status(page->app, "已选择启动环境 %s", name);
    be_update_panel(page);
}

/* ------------------------------------------------------------------------- */
/* 对象操作面板                                                               */
/* ------------------------------------------------------------------------- */

static void
on_be_action_done(gboolean success,
                  gpointer user_data)
{
    (void)success;
    (void)user_data;
    be_refresh(user_data);
}

static void
on_be_create_name(const gchar *text,
                  gpointer data)
{
    BePage *page = data;
    ZgArgs *args;
    gchar *name = zg_str_trim(text);
    gchar *message;

    if (!zg_simple_name_valid(name))
    {
        zg_dialogs_error(page_window(page), "名称非法",
                         "启动环境名称只能包含字母、数字、下划线、点和连字符。");
        g_free(name);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, name);

    message = g_strdup_printf("启动环境 %s 已创建。", name);

    zg_dialogs_run_op(page_window(page), "bectl.create", args, message,
                      on_be_action_done, page);
    g_free(message);
    g_free(name);
}

static void
on_be_create(GtkButton *button,
             gpointer user_data)
{
    BePage *page = user_data;
    GDateTime *now = g_date_time_new_now_local();
    gchar *default_name = g_date_time_format(now, "be-%Y%m%d-%H%M%S");

    (void)button;

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "创建启动环境",
                                    .heading = "新建启动环境",
                                    .description = "启动环境是系统根数据集的快照副本，可在升级失败后一键回退。",
                                    .field_label = "启动环境名称",
                                    .placeholder = "be-backup",
                                    .default_value = default_name,
                                    .accept_label = "创建",
                                    .destructive = FALSE},
                     on_be_create_name, page);

    g_free(default_name);
    g_date_time_unref(now);
}

static void
on_be_rename_name(const gchar *text,
                  gpointer data)
{
    BePage *page = data;
    ZgArgs *args;
    gchar *name = zg_str_trim(text);
    gchar *message;

    if (!zg_simple_name_valid(name))
    {
        zg_dialogs_error(page_window(page), "名称非法", name);
        g_free(name);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);
    zg_args_add_positional(args, name);

    message = g_strdup_printf("启动环境已重命名为 %s。", name);

    zg_dialogs_run_op(page_window(page), "bectl.rename", args, message,
                      on_be_action_done, page);
    g_free(message);
    g_free(name);
}

static void
on_be_rename(GtkButton *button,
             gpointer user_data)
{
    BePage *page = user_data;

    (void)button;

    if (page->selected == NULL)
        return;

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "重命名启动环境",
                                    .heading = "重命名",
                                    .description = "重命名不会改变启动环境的内容。",
                                    .field_label = "新名称",
                                    .placeholder = "be-new",
                                    .default_value = page->selected,
                                    .accept_label = "重命名",
                                    .destructive = FALSE},
                     on_be_rename_name, page);
}

static void
on_be_activate_clicked(GtkButton *button,
                       gpointer user_data)
{
    BePage *page = user_data;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (page->selected == NULL)
        return;

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);

    message = g_strdup_printf("启动环境 %s 已设为下次启动目标。", page->selected);

    zg_dialogs_run_op(page_window(page), "bectl.activate", args, message,
                      on_be_action_done, page);
    g_free(message);
}

static void
on_be_mount_clicked(GtkButton *button,
                    gpointer user_data)
{
    BePage *page = user_data;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (page->selected == NULL)
        return;

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);

    message = g_strdup_printf("启动环境 %s 已挂载到默认挂载点。", page->selected);

    zg_dialogs_run_op(page_window(page), "bectl.mount", args, message,
                      on_be_action_done, page);
    g_free(message);
}

static void
on_be_unmount_clicked(GtkButton *button,
                      gpointer user_data)
{
    BePage *page = user_data;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (page->selected == NULL)
        return;

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);

    message = g_strdup_printf("启动环境 %s 已卸载。", page->selected);

    zg_dialogs_run_op(page_window(page), "bectl.unmount", args, message,
                      on_be_action_done, page);
    g_free(message);
}

static void
on_be_export_clicked(GtkButton *button,
                     gpointer user_data)
{
    BePage *page = user_data;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (page->selected == NULL)
        return;

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);

    message = g_strdup_printf("启动环境 %s 已导出。", page->selected);

    zg_dialogs_run_op(page_window(page), "bectl.export", args, message,
                      on_be_action_done, page);
    g_free(message);
}

static void
on_be_destroy_clicked(GtkButton *button,
                      gpointer user_data)
{
    BePage *page = user_data;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (page->selected == NULL)
        return;

    args = zg_args_new();
    zg_args_add_positional(args, page->selected);

    message = g_strdup_printf("启动环境 %s 已销毁。", page->selected);

    zg_dialogs_run_op(page_window(page), "bectl.destroy", args, message,
                      on_be_action_done, page);
    g_free(message);
}

static void
on_be_refresh(GtkButton *button,
              gpointer user_data)
{
    (void)button;

    zg_app_refresh_all(zg_app_get_default());
}

static void
be_update_panel(BePage *page)
{
    const gchar *hint =
            "点击左侧启动环境查看操作 · Enter 激活 · Delete 销毁 · F5 刷新";

    if (page->selected == NULL)
    {
        ZgAction actions[] = {
                {"刷新", "view-refresh-symbolic", "重新读取启动环境列表（F5）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_be_refresh), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "applications-system-symbolic",
                                           .title = "启动环境",
                                           .subtitle = "选择一个启动环境开始操作",
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = "提示：先在左侧点一个启动环境，再点上面的按钮操作它"});
        return;
    }

    {
        ZgAction actions[] = {
                {"激活", "emblem-ok-symbolic", "设为下次启动目标（Enter）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_be_activate_clicked), page},
                {"挂载", "media-playback-start-symbolic", "挂载以便修改",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_be_mount_clicked), page},
                {"卸载", "media-playback-stop-symbolic", "卸载启动环境",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_be_unmount_clicked), page},
                {"重命名", "document-edit-symbolic", "重命名启动环境",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_be_rename), page},
                {"导出", "document-save-symbolic", "导出为归档",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_be_export_clicked), page},
                {"销毁", "edit-delete-symbolic", "删除该启动环境（Delete）",
                 TRUE, FALSE, TRUE, G_CALLBACK(on_be_destroy_clicked), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "applications-system-symbolic",
                                           .title = page->selected,
                                           .subtitle = "ZFS 启动环境（bectl）",
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = hint});
    }
}

static void
on_table_activated(GtkColumnView *view,
                   guint position,
                   gpointer user_data)
{
    (void)view;
    (void)position;

    on_be_activate_clicked(NULL, user_data);
}

static gboolean
on_be_key(GtkEventControllerKey *controller,
          guint keyval,
          guint keycode,
          GdkModifierType state,
          gpointer user_data)
{
    BePage *page = user_data;

    (void)controller;
    (void)keycode;
    (void)state;

    switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        on_be_activate_clicked(NULL, page);
        return TRUE;
    case GDK_KEY_Delete:
        on_be_destroy_clicked(NULL, page);
        return TRUE;
    case GDK_KEY_F5:
        zg_app_refresh_all(page->app);
        return TRUE;
    default:
        return FALSE;
    }
}

GtkWidget *
zg_page_be_new(ZgApp *app)
{
    BePage *page = g_new0(BePage, 1);
    GtkWidget *toolbar;
    GtkWidget *scrolled;
    GtkWidget *box;
    GtkWidget *hint;

    page->app = app;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    page->panel = zg_action_panel_new();
    gtk_box_append(GTK_BOX(box), page->panel);

    toolbar = zg_ui_toolbar_new();
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("创建启动环境", G_CALLBACK(on_be_create), page));
    gtk_box_append(GTK_BOX(box), toolbar);

    hint = zg_ui_label_dim(
            "启动环境基于 zroot/ROOT 数据集，可在升级失败后快速回退系统版本。"
            "「激活」只影响下次启动，重启后生效。"
            "启动环境的快照（名称含 @）请在「快照」页管理。");
    gtk_widget_set_margin_top(hint, 6);
    gtk_widget_set_margin_bottom(hint, 6);
    gtk_widget_set_margin_start(hint, 10);
    gtk_widget_set_margin_end(hint, 10);
    gtk_box_append(GTK_BOX(box), hint);

    page->store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"启动环境", BE_COL_NAME, TRUE, 20, 0.0f, TRUE, FALSE},
                {"活动", BE_COL_ACTIVE, FALSE, 6, 0.0f, FALSE, FALSE},
                {"挂载点", BE_COL_MOUNT, FALSE, 12, 0.0f, FALSE, FALSE},
                {"占用", BE_COL_SPACE, FALSE, 8, 1.0f, FALSE, FALSE},
                {"创建时间", BE_COL_CREATED, FALSE, 18, 0.0f, FALSE, TRUE},
        };

        page->table = zg_table_new_flat(page->store, columns,
                                        G_N_ELEMENTS(columns));
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->table);
    /* 列较多时允许横向滚动，避免目录树被裁切 */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), FALSE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    {
        GtkSelectionModel *selection =
                zg_table_get_selection(page->table);

        g_signal_connect(selection, "selection-changed",
                         G_CALLBACK(on_be_selection_changed), page);
        g_signal_connect(page->table, "activate",
                         G_CALLBACK(on_table_activated), page);
    }

    {
        GtkEventController *keys = gtk_event_controller_key_new();

        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_be_key), page);
        gtk_widget_add_controller(page->table, keys);
    }

    page->root = box;

    g_object_set_data_full(G_OBJECT(page->root), "zg-page-be", page, g_free);

    zg_app_register_refresh(app, be_refresh, page);
    be_update_panel(page);

    return page->root;
}
