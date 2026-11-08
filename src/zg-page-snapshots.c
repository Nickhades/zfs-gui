/* zg-page-snapshots.c - 快照页面
 *
 * 列出全部快照，支持创建、回滚、删除、克隆、发送、差异比较与 hold 管理。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-actions.h"
#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-model.h"
#include "zg-pages.h"
#include "zg-table.h"
#include "zg-ui.h"
#include "zg-util.h"

enum
{
    SNAP_COL_NAME,
    SNAP_COL_DATASET,
    SNAP_COL_USED,
    SNAP_COL_REFER,
    SNAP_COL_CREATED,
    SNAP_COL_FULLNAME,
    SNAP_N_COLUMNS
};

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GListStore *store;
    GtkWidget *table;
    GtkTextBuffer *holds_buffer;
    GtkWidget *panel;
    GPtrArray *snapshots;
    gchar *selected;
    gchar *sel_snapshot;
    gchar *sel_dataset;
} SnapshotsPage;

static void snapshots_update_panel(SnapshotsPage *page);
static void on_table_activated(GtkColumnView *view, guint position, gpointer user_data);
static void on_snap_rollback(GtkButton *button, gpointer user_data);
static void on_snap_send(GtkButton *button, gpointer user_data);
static void on_snap_clone(GtkButton *button, gpointer user_data);
static void on_snap_diff(GtkButton *button, gpointer user_data);
static void on_snap_hold(GtkButton *button, gpointer user_data);
static void on_snap_release(GtkButton *button, gpointer user_data);
static void on_snap_bookmark(GtkButton *button, gpointer user_data);
static void on_snap_destroy(GtkButton *button, gpointer user_data);

typedef struct
{
    gchar *name;
    gchar *dataset;
} SnapTag;

static void
snap_tag_free(gpointer data)
{
    SnapTag *tag = data;

    if (tag == NULL)
        return;

    g_free(tag->name);
    g_free(tag->dataset);
    g_free(tag);
}

static GtkWindow *
page_window(SnapshotsPage *page)
{
    return GTK_WINDOW(gtk_widget_get_root(page->root));
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

static void
snapshots_rebuild(SnapshotsPage *page)
{
    guint i;

    g_list_store_remove_all(page->store);

    for (i = 0; page->snapshots != NULL && i < page->snapshots->len; i++)
    {
        ZgSnapshot *snapshot = g_ptr_array_index(page->snapshots, i);
        ZgRow *row = zg_row_new();
        SnapTag *tag = g_new0(SnapTag, 1);
        gchar *used;
        gchar *refer;
        gchar *created;

        used = zg_format_bytes(snapshot->used);
        refer = zg_format_bytes(snapshot->refer);
        created = zg_format_epoch_local(snapshot->creation);

        zg_row_set_text(row, SNAP_COL_NAME, snapshot->snap);
        zg_row_set_text(row, SNAP_COL_DATASET, snapshot->dataset);
        zg_row_set_text(row, SNAP_COL_USED, used);
        zg_row_set_text(row, SNAP_COL_REFER, refer);
        zg_row_set_text(row, SNAP_COL_CREATED, created);

        tag->name = g_strdup(snapshot->name);
        tag->dataset = g_strdup(snapshot->dataset);
        zg_row_set_tag(row, tag, snap_tag_free);

        g_list_store_append(page->store, row);
        g_object_unref(row);

        g_free(used);
        g_free(refer);
        g_free(created);
    }

    zg_table_select_first(page->table);
}

static void
on_snapshots_list(GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
    SnapshotsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
    {
        if (page->snapshots != NULL)
            g_ptr_array_free(page->snapshots, TRUE);

        page->snapshots = zg_model_snapshots_from_tsv(response->stdout_text);
        snapshots_rebuild(page);

        zg_app_set_status(page->app, "快照列表已更新（%u 个）",
                          page->snapshots != NULL ? page->snapshots->len : 0);
    }
    else
    {
        zg_app_set_status(page->app, "读取快照失败：%s",
                          response != NULL ? response->message : "无应答");
    }

    zg_client_result_free(response);
}

static void
snapshots_refresh(gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_option(args, "t", "snapshot");
    zg_args_add_option(args, "o", ZG_MODEL_SNAPSHOT_FIELDS);

    zg_client_call_async("zfs.list", zg_args_to_json(args),
                         on_snapshots_list, page);
}

/* ------------------------------------------------------------------------- */
/* holds 详情                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
    SnapshotsPage *page;
    gchar *snapshot;
} HoldsContext;

static void
on_holds(GObject *source,
         GAsyncResult *result,
         gpointer user_data)
{
    HoldsContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (g_strcmp0(context->page->selected, context->snapshot) == 0)
        zg_ui_text_set(context->page->holds_buffer,
                       (response != NULL && response->ok && response->stdout_text[0] != '\0')
                               ? response->stdout_text
                               : "（没有 hold 标签，快照可被销毁）");

    zg_client_result_free(response);
    g_free(context->snapshot);
    g_free(context);
}

static void
snapshots_load_holds(SnapshotsPage *page,
                     const gchar *snapshot)
{
    HoldsContext *context;
    ZgArgs *args;

    if (snapshot == NULL)
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_positional(args, snapshot);

    context = g_new0(HoldsContext, 1);
    context->page = page;
    context->snapshot = g_strdup(snapshot);

    zg_client_call_async("zfs.holds", zg_args_to_json(args), on_holds, context);
}

static void
on_selection_changed(GtkSelectionModel *selection,
                     guint position,
                     guint n_items,
                     gpointer user_data)
{
    SnapshotsPage *page = user_data;
    ZgRow *row;
    SnapTag *tag;

    (void)selection;
    (void)position;
    (void)n_items;

    row = zg_table_get_selected(page->table);
    tag = row != NULL ? zg_row_get_tag(row) : NULL;

    if (tag == NULL)
        return;

    g_free(page->selected);
    page->selected = g_strdup(tag->name);
    g_free(page->sel_snapshot);
    page->sel_snapshot = g_strdup(tag->name);
    g_free(page->sel_dataset);
    page->sel_dataset = g_strdup(tag->dataset);

    snapshots_load_holds(page, tag->name);
    zg_app_set_status(page->app, "已选择快照 %s", tag->name);
    snapshots_update_panel(page);
}

/* ------------------------------------------------------------------------- */
/* 操作                                                                       */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 右键菜单                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 构建界面                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 对象操作面板                                                               */
/* ------------------------------------------------------------------------- */

static void
on_snap_action_done(gboolean success,
                    gpointer user_data)
{
    (void)success;
    (void)user_data;
}

static void
on_snap_create(GtkButton *button,
               gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    zg_dialogs_create_snapshot(page_window(page), page->sel_dataset,
                               on_snap_action_done, page);
}

static void
on_snap_send(GtkButton *button,
             gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    if (page->sel_snapshot != NULL)
        zg_dialogs_send_snapshot(page_window(page), page->sel_snapshot,
                                 on_snap_action_done, page);
}

static void
on_snap_destroy(GtkButton *button,
                gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    if (page->sel_snapshot != NULL)
        zg_dialogs_destroy_target(page_window(page), page->sel_snapshot, FALSE,
                                  on_snap_action_done, page);
}

static void
on_snap_refresh(GtkButton *button,
                gpointer user_data)
{
    (void)button;

    zg_app_refresh_all(zg_app_get_default());
}

static void
on_rollback_choice(const gboolean *selected,
                   gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args = zg_args_new();
    gchar *message;

    zg_args_add_positional(args, page->sel_snapshot);

    if (selected[0])
        zg_args_add_flag(args, "-r");

    if (selected[1])
        zg_args_add_flag(args, "-f");

    message = g_strdup_printf("已回滚到 %s。", page->sel_snapshot);

    zg_dialogs_run_op(page_window(page), "zfs.rollback", args, message,
                      on_snap_action_done, page);
    g_free(message);
}

static void
on_snap_rollback(GtkButton *button,
                 gpointer user_data)
{
    SnapshotsPage *page = user_data;
    ZgChoice choices[2];
    gchar *description;

    (void)button;

    description = g_strdup_printf(
            "回滚会把数据集恢复到快照 %s 的状态，该快照之后写入的数据将丢失。\n"
            "该操作不可撤销，且需要管理员密码确认。",
            page->sel_snapshot);

    choices[0].label = "同时销毁该快照之后创建的所有快照（-r）";
    choices[0].hint = "不勾选时，若存在更新的快照会导致回滚失败";
    choices[0].active = FALSE;

    choices[1].label = "强制卸载正在使用的文件系统（-f）";
    choices[1].hint = "用于数据集正被挂载或 busy 的情况";
    choices[1].active = FALSE;

    zg_dialogs_choice(page_window(page),
                      &(ZgChoiceSpec){.title = "回滚到快照",
                                      .heading = "回滚数据集",
                                      .description = description,
                                      .choices = choices,
                                      .n_choices = 2,
                                      .accept_label = "确认回滚",
                                      .destructive = TRUE},
                      on_rollback_choice, page);
    g_free(description);
}

static void
on_clone_name(const gchar *text,
              gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args;
    gchar *message;

    if (!zg_dataset_name_valid(text))
    {
        zg_dialogs_error(page_window(page), "名称非法",
                         "新数据集名称应形如 tank/restore。");
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, page->sel_snapshot);
    zg_args_add_positional(args, text);

    message = g_strdup_printf("已从快照克隆出 %s。", text);

    zg_dialogs_run_op(page_window(page), "zfs.clone", args, message,
                      on_snap_action_done, page);
    g_free(message);
}

static void
on_snap_clone(GtkButton *button,
              gpointer user_data)
{
    SnapshotsPage *page = user_data;
    gchar *default_name;

    (void)button;

    default_name = g_strdup_printf("%s/restore", page->sel_dataset);

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "克隆快照",
                                    .heading = "克隆为新数据集",
                                    .description = "克隆会创建一个可写的数据集，其初始内容来自该快照。",
                                    .field_label = "新数据集名称",
                                    .placeholder = "tank/restore",
                                    .default_value = default_name,
                                    .accept_label = "创建克隆",
                                    .destructive = FALSE},
                     on_clone_name, page);
    g_free(default_name);
}

static void
on_diff_target(const gchar *text,
               gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args = zg_args_new();
    gchar *trimmed = zg_str_trim(text);

    zg_args_add_flag(args, "-H");
    zg_args_add_positional(args, page->sel_snapshot);

    if (trimmed[0] != '\0')
        zg_args_add_positional(args, trimmed);

    zg_dialogs_run_op(page_window(page), "zfs.diff", args, NULL,
                      on_snap_action_done, page);

    g_free(trimmed);
}

static void
on_snap_diff(GtkButton *button,
             gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "比较快照差异",
                                    .heading = "比较文件差异",
                                    .description = "留空则与该快照对应的数据集当前状态比较；也可以填写另一个快照名称。",
                                    .field_label = "对比目标（可选）",
                                    .placeholder = "tank/data@other",
                                    .default_value = "",
                                    .accept_label = "开始比较",
                                    .destructive = FALSE},
                     on_diff_target, page);
}

static void
on_hold_tag(const gchar *text,
            gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args = zg_args_new();
    gchar *tag = zg_str_trim(text);
    gchar *message;

    if (tag[0] == '\0')
    {
        zg_dialogs_error(page_window(page), "标签不能为空", "请填写 hold 标签。");
        g_free(tag);
        return;
    }

    zg_args_add_positional(args, tag);
    zg_args_add_positional(args, page->sel_snapshot);

    message = g_strdup_printf("快照已持有（hold: %s），不会被误删。", tag);

    zg_dialogs_run_op(page_window(page), "zfs.hold", args, message,
                      on_snap_action_done, page);
    g_free(message);
    g_free(tag);
}

static void
on_snap_hold(GtkButton *button,
             gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "持有快照",
                                    .heading = "为快照添加 hold 标签",
                                    .description = "带 hold 的快照无法被销毁，适合长期保留的备份点。",
                                    .field_label = "标签",
                                    .placeholder = "keep",
                                    .default_value = "keep",
                                    .accept_label = "添加 hold",
                                    .destructive = FALSE},
                     on_hold_tag, page);
}

static void
on_release_tag(const gchar *text,
               gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args = zg_args_new();
    gchar *tag = zg_str_trim(text);
    gchar *message;

    if (tag[0] == '\0')
    {
        zg_dialogs_error(page_window(page), "标签不能为空", "请填写要释放的标签。");
        g_free(tag);
        return;
    }

    zg_args_add_positional(args, tag);
    zg_args_add_positional(args, page->sel_snapshot);

    message = g_strdup_printf("已释放 hold: %s。", tag);

    zg_dialogs_run_op(page_window(page), "zfs.release", args, message,
                      on_snap_action_done, page);
    g_free(message);
    g_free(tag);
}

static void
on_snap_release(GtkButton *button,
                gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)button;

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "释放快照",
                                    .heading = "移除快照的 hold 标签",
                                    .description = "释放后该快照可以被销毁。",
                                    .field_label = "标签",
                                    .placeholder = "keep",
                                    .default_value = "keep",
                                    .accept_label = "释放 hold",
                                    .destructive = FALSE},
                     on_release_tag, page);
}

static void
on_bookmark_name(const gchar *text,
                 gpointer data)
{
    SnapshotsPage *page = data;
    ZgArgs *args = zg_args_new();
    gchar *name = zg_str_trim(text);
    gchar *message;

    if (!zg_snapshot_name_valid(name))
    {
        zg_dialogs_error(page_window(page), "书签名称非法",
                         "书签应形如 tank/data#keep。");
        g_free(name);
        return;
    }

    zg_args_add_positional(args, page->sel_snapshot);
    zg_args_add_positional(args, name);

    message = g_strdup_printf("书签 %s 已创建。", name);

    zg_dialogs_run_op(page_window(page), "zfs.bookmark", args, message,
                      on_snap_action_done, page);
    g_free(message);
    g_free(name);
}

static void
on_snap_bookmark(GtkButton *button,
                 gpointer user_data)
{
    SnapshotsPage *page = user_data;
    gchar *default_name;

    (void)button;

    default_name = g_strdup_printf("%s#keep", page->sel_dataset);

    zg_dialogs_input(page_window(page),
                     &(ZgInputSpec){.title = "创建书签",
                                    .heading = "把快照标记为书签",
                                    .description = "书签不占空间，销毁快照后仍可用于增量发送。",
                                    .field_label = "书签名称",
                                    .placeholder = "tank/data#keep",
                                    .default_value = default_name,
                                    .accept_label = "创建书签",
                                    .destructive = FALSE},
                     on_bookmark_name, page);
    g_free(default_name);
}

static void
snapshots_update_panel(SnapshotsPage *page)
{
    const gchar *hint =
            "点击左侧快照查看操作 · Enter 发送 · Delete 销毁 · F5 刷新";

    if (page->sel_snapshot == NULL)
    {
        ZgAction actions[] = {
                {"刷新", "view-refresh-symbolic", "重新读取列表（F5）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_snap_refresh), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "camera-photo-symbolic",
                                           .title = "快照",
                                           .subtitle = "选择一个快照开始操作",
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = "提示：在左侧点一个对象，再用上方按钮操作；创建/导入见上方工具栏"});
        return;
    }

    {
        ZgAction actions[] = {
                {"发送", "document-send-symbolic", "导出为快照流文件（Enter）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_snap_send), page},
                {"回滚", "edit-undo-symbolic", "把数据集恢复到该快照",
                 TRUE, FALSE, TRUE, G_CALLBACK(on_snap_rollback), page},
                {"克隆", "edit-copy-symbolic", "克隆为新的可写数据集",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_snap_clone), page},
                {"差异", "edit-find-symbolic", "比较文件差异",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_snap_diff), page},
                {"hold", "bookmark-new-symbolic", "防止快照被误删",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_snap_hold), page},
                {"release", "user-trash-symbolic", "释放 hold 标签",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_snap_release), page},
                {"书签", "bookmark-new-symbolic", "创建不占空间的增量基点",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_snap_bookmark), page},
                {"销毁", "edit-delete-symbolic", "永久删除该快照（Delete）",
                 TRUE, FALSE, TRUE, G_CALLBACK(on_snap_destroy), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "camera-photo-symbolic",
                                           .title = page->selected,
                                           .subtitle = page->sel_dataset != NULL ? page->sel_dataset : "",
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

    on_snap_send(NULL, user_data);
}

static gboolean
on_snapshot_key(GtkEventControllerKey *controller,
                guint keyval,
                guint keycode,
                GdkModifierType state,
                gpointer user_data)
{
    SnapshotsPage *page = user_data;

    (void)controller;
    (void)keycode;
    (void)state;

    switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        on_snap_send(NULL, page);
        return TRUE;
    case GDK_KEY_Delete:
        on_snap_destroy(NULL, page);
        return TRUE;
    case GDK_KEY_F5:
        zg_app_refresh_all(page->app);
        return TRUE;
    default:
        return FALSE;
    }
}

GtkWidget *
zg_page_snapshots_new(ZgApp *app)
{
    SnapshotsPage *page = g_new0(SnapshotsPage, 1);
    GtkWidget *paned;
    GtkWidget *left;
    GtkWidget *toolbar;
    GtkWidget *scrolled;
    GtkWidget *holds;
    GtkSelectionModel *selection;

    page->app = app;

    page->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    page->panel = zg_action_panel_new();
    gtk_box_append(GTK_BOX(page->root), page->panel);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(page->root), paned);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    toolbar = zg_ui_toolbar_new();
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("创建快照", G_CALLBACK(on_snap_create), page));
    gtk_box_append(GTK_BOX(left), toolbar);

    {
        GtkWidget *hint = zg_ui_label_wrap(
                "快照始终是完整时间点：「独占空间」为该快照额外占用的空间，"
                "新建时为 0B 属正常；「包含数据」是它能恢复的数据总量。");
        gtk_widget_add_css_class(hint, "zg-dim");
        gtk_widget_set_margin_top(hint, 6);
        gtk_widget_set_margin_bottom(hint, 6);
        gtk_widget_set_margin_start(hint, 12);
        gtk_widget_set_margin_end(hint, 12);
        gtk_box_append(GTK_BOX(left), hint);
    }

    page->store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"快照", SNAP_COL_NAME, TRUE, 20, 0.0f, TRUE, FALSE},
                {"数据集", SNAP_COL_DATASET, TRUE, 16, 0.0f, FALSE, FALSE},
                {"独占空间", SNAP_COL_USED, FALSE, 9, 1.0f, FALSE, FALSE},
                {"包含数据", SNAP_COL_REFER, FALSE, 9, 1.0f, FALSE, FALSE},
                {"创建时间", SNAP_COL_CREATED, FALSE, 18, 0.0f, FALSE, TRUE},
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
    gtk_box_append(GTK_BOX(left), scrolled);

    selection = zg_table_get_selection(page->table);
    g_signal_connect(selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), page);
    g_signal_connect(page->table, "activate", G_CALLBACK(on_table_activated), page);

    {
        GtkEventController *keys = gtk_event_controller_key_new();

        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_snapshot_key), page);
        gtk_widget_add_controller(page->table, keys);
    }

    holds = zg_ui_text_view_new(&page->holds_buffer, FALSE);
    gtk_paned_set_end_child(GTK_PANED(paned), holds);

    gtk_paned_set_position(GTK_PANED(paned), 900);

    g_object_set_data_full(G_OBJECT(page->root), "zg-page-snapshots", page, g_free);

    zg_app_register_refresh(app, snapshots_refresh, page);
    snapshots_update_panel(page);

    return page->root;
}
