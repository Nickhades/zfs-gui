/* zg-page-datasets.c - 数据集页面
 *
 * 左侧：数据集层级树
 * 右侧：属性 / 用户配额 / 组配额 / 挂载信息
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
    DS_COL_NAME,
    DS_COL_TYPE,
    DS_COL_USED,
    DS_COL_AVAIL,
    DS_COL_REFER,
    DS_COL_MOUNT,
    DS_COL_CREATED,
    DS_COL_FULLNAME,
    DS_COL_MOUNTED,
    DS_N_COLUMNS
};

enum
{
    DPROP_COL_NAME,
    DPROP_COL_VALUE,
    DPROP_COL_SOURCE,
    DPROP_N_COLUMNS
};

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GListStore *store;
    GtkWidget *table;
    GListStore *props_store;
    GtkWidget *props_table;
    GtkWidget *panel;
    GtkWidget *notebook;
    GtkTextBuffer *userspace_buffer;
    GtkTextBuffer *groupspace_buffer;

    GPtrArray *datasets;
    gchar *selected;
    gchar *sel_name;
    gchar *sel_type;
    gboolean sel_mounted;
} DatasetsPage;

static void datasets_update_panel(DatasetsPage *page);
static void on_table_activated(GtkColumnView *view, guint position, gpointer user_data);
static void on_ds_show_props(GtkButton *button, gpointer user_data);

typedef struct
{
    gchar *fullname;
} DsTag;

static void
ds_tag_free(gpointer data)
{
    DsTag *tag = data;

    if (tag == NULL)
        return;

    g_free(tag->fullname);
    g_free(tag);
}

/* ------------------------------------------------------------------------- */
/* 选择                                                                       */
/* ------------------------------------------------------------------------- */

static gchar *
selected_dataset(DatasetsPage *page)
{
    ZgRow *row = zg_table_get_selected(page->table);
    DsTag *tag;

    if (row == NULL)
        return NULL;

    tag = zg_row_get_tag(row);

    return tag != NULL ? g_strdup(tag->fullname) : NULL;
}

static GtkWindow *
page_window(DatasetsPage *page)
{
    return GTK_WINDOW(gtk_widget_get_root(page->root));
}

/* ------------------------------------------------------------------------- */
/* 视图构建                                                                   */
/* ------------------------------------------------------------------------- */

static gint
compare_datasets(gconstpointer a,
                 gconstpointer b)
{
    const ZgDataset *left = *(ZgDataset *const *)a;
    const ZgDataset *right = *(ZgDataset *const *)b;

    return g_strcmp0(left->name, right->name);
}
static void
datasets_rebuild(DatasetsPage *page)
{
    GHashTable *row_table;
    guint i;

    g_list_store_remove_all(page->store);

    if (page->datasets != NULL)
        g_ptr_array_sort(page->datasets, compare_datasets);

    row_table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                      g_free, g_object_unref);

    /* 第一遍：创建全部行，先不放进模型。树模型会在首次看到父行时缓存
     * “是否有子节点”，因此必须先把父子关系挂好，再挂根节点。 */
    for (i = 0; page->datasets != NULL && i < page->datasets->len; i++)
    {
        ZgDataset *dataset = g_ptr_array_index(page->datasets, i);
        ZgRow *row = zg_row_new();
        DsTag *tag;
        gchar *used;
        gchar *avail;
        gchar *refer;
        gchar *created;
        const gchar *leaf;
        gchar *slash;

        slash = strrchr(dataset->name, '/');
        leaf = (slash != NULL) ? slash + 1 : dataset->name;

        used = zg_format_bytes(dataset->used);
        avail = zg_format_bytes(dataset->avail);
        refer = zg_format_bytes(dataset->refer);
        created = zg_format_epoch_local(dataset->creation);

        zg_row_set_text(row, DS_COL_NAME, leaf);
        zg_row_set_text(row, DS_COL_TYPE, dataset->type);
        zg_row_set_text(row, DS_COL_USED, used);
        zg_row_set_text(row, DS_COL_AVAIL, avail);
        zg_row_set_text(row, DS_COL_REFER, refer);
        zg_row_set_text(row, DS_COL_MOUNT, dataset->mountpoint);
        zg_row_set_text(row, DS_COL_CREATED, created);
        zg_row_set_text(row, DS_COL_MOUNTED, dataset->mounted ? "1" : "0");

        tag = g_new0(DsTag, 1);
        tag->fullname = g_strdup(dataset->name);
        zg_row_set_tag(row, tag, ds_tag_free);

        g_hash_table_insert(row_table, g_strdup(dataset->name),
                            g_object_ref_sink(row));

        g_free(used);
        g_free(avail);
        g_free(refer);
        g_free(created);
    }

    /* 第二遍：先把所有子节点挂到父节点，保证树模型看到根节点时子节点已存在 */
    for (i = 0; page->datasets != NULL && i < page->datasets->len; i++)
    {
        ZgDataset *dataset = g_ptr_array_index(page->datasets, i);
        ZgRow *row = g_hash_table_lookup(row_table, dataset->name);
        gchar *slash;
        gchar *parent_name;
        ZgRow *parent_row;

        if (row == NULL)
            continue;

        slash = strrchr(dataset->name, '/');
        if (slash == NULL)
            continue;

        parent_name = g_strndup(dataset->name, (gsize)(slash - dataset->name));
        parent_row = g_hash_table_lookup(row_table, parent_name);

        if (parent_row != NULL && parent_row != row)
            zg_row_add_child(parent_row, row);

        g_free(parent_name);
    }

    /* 第三遍：只把根节点放进模型 */
    for (i = 0; page->datasets != NULL && i < page->datasets->len; i++)
    {
        ZgDataset *dataset = g_ptr_array_index(page->datasets, i);
        ZgRow *row = g_hash_table_lookup(row_table, dataset->name);

        if (row != NULL && strchr(dataset->name, '/') == NULL)
            g_list_store_append(page->store, row);
    }

    g_hash_table_destroy(row_table);

    zg_table_select_first(page->table);
}

/* ------------------------------------------------------------------------- */
/* 详情                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    DatasetsPage *page;
    gchar *dataset;
} DsContext;

static void
ds_context_free(gpointer data)
{
    DsContext *context = data;

    if (context == NULL)
        return;

    g_free(context->dataset);
    g_free(context);
}

static gboolean
ds_is_current(DsContext *context)
{
    return context->page->selected != NULL &&
           g_strcmp0(context->page->selected, context->dataset) == 0;
}

static void
on_ds_props(GObject *source,
            GAsyncResult *result,
            gpointer user_data)
{
    DsContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GPtrArray *properties;
    guint i;

    (void)source;

    if (ds_is_current(context))
    {
        g_list_store_remove_all(context->page->props_store);

        properties = zg_model_properties_from_tsv(
                (response != NULL && response->ok) ? response->stdout_text : "");

        for (i = 0; i < properties->len; i++)
        {
            ZgProperty *property = g_ptr_array_index(properties, i);
            ZgRow *row = zg_row_new();

            zg_row_set_text(row, DPROP_COL_NAME, property->property);
            zg_row_set_text(row, DPROP_COL_VALUE, property->value);
            zg_row_set_text(row, DPROP_COL_SOURCE, property->source);

            g_list_store_append(context->page->props_store, row);
            g_object_unref(row);
        }

        g_ptr_array_free(properties, TRUE);
    }

    zg_client_result_free(response);
    ds_context_free(context);
}

static void
on_ds_userspace(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
    DsContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (ds_is_current(context))
    {
        if (response == NULL)
            zg_ui_text_set(context->page->userspace_buffer, "后台服务无应答");
        else if (!response->ok)
            zg_ui_text_set(context->page->userspace_buffer, response->message);
        else if (response->stdout_text[0] == '\0')
            zg_ui_text_set(context->page->userspace_buffer, "（没有用户配额记录）");
        else
            zg_ui_text_set(context->page->userspace_buffer, response->stdout_text);
    }

    zg_client_result_free(response);
    ds_context_free(context);
}

static void
on_ds_groupspace(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DsContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (ds_is_current(context))
    {
        if (response == NULL)
            zg_ui_text_set(context->page->groupspace_buffer, "后台服务无应答");
        else if (!response->ok)
            zg_ui_text_set(context->page->groupspace_buffer, response->message);
        else if (response->stdout_text[0] == '\0')
            zg_ui_text_set(context->page->groupspace_buffer, "（没有组配额记录）");
        else
            zg_ui_text_set(context->page->groupspace_buffer, response->stdout_text);
    }

    zg_client_result_free(response);
    ds_context_free(context);
}

static void
datasets_load_detail(DatasetsPage *page,
                     const gchar *dataset)
{
    DsContext *context;
    ZgArgs *args;

    if (dataset == NULL)
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_positional(args, "all");
    zg_args_add_positional(args, dataset);

    context = g_new0(DsContext, 1);
    context->page = page;
    context->dataset = g_strdup(dataset);
    zg_client_call_async("zfs.get", zg_args_to_json(args), on_ds_props, context);

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_positional(args, dataset);

    context = g_new0(DsContext, 1);
    context->page = page;
    context->dataset = g_strdup(dataset);
    zg_client_call_async("zfs.userspace", zg_args_to_json(args), on_ds_userspace, context);

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_positional(args, dataset);

    context = g_new0(DsContext, 1);
    context->page = page;
    context->dataset = g_strdup(dataset);
    zg_client_call_async("zfs.groupspace", zg_args_to_json(args), on_ds_groupspace, context);
}
static void
on_selection_changed(GtkSelectionModel *selection,
                     guint position,
                     guint n_items,
                     gpointer user_data)
{
    DatasetsPage *page = user_data;
    ZgRow *row;
    DsTag *tag;
    const gchar *name;
    const gchar *type;
    gboolean mounted;

    (void)selection;
    (void)position;
    (void)n_items;

    row = zg_table_get_selected(page->table);
    tag = row != NULL ? zg_row_get_tag(row) : NULL;

    if (tag == NULL || tag->fullname == NULL)
        return;

    name = tag->fullname;
    type = zg_row_get_text(row, DS_COL_TYPE);
    mounted = g_strcmp0(zg_row_get_text(row, DS_COL_MOUNTED), "1") == 0;

    g_free(page->selected);
    page->selected = g_strdup(name);
    g_free(page->sel_name);
    page->sel_name = g_strdup(name);
    g_free(page->sel_type);
    page->sel_type = g_strdup(type);
    page->sel_mounted = mounted;

    datasets_load_detail(page, name);

    zg_app_set_status(page->app, "已选择数据集 %s", name);
    datasets_update_panel(page);
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_datasets_list(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DatasetsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
    {
        if (page->datasets != NULL)
            g_ptr_array_free(page->datasets, TRUE);

        page->datasets = zg_model_datasets_from_tsv(response->stdout_text);
        datasets_rebuild(page);

        zg_app_set_status(page->app, "数据集列表已更新（%u 项）",
                          page->datasets != NULL ? page->datasets->len : 0);
    }
    else
    {
        zg_app_set_status(page->app, "读取数据集失败：%s",
                          response != NULL ? response->message : "无应答");
    }

    zg_client_result_free(response);
}

static void
datasets_refresh(gpointer data)
{
    DatasetsPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_option(args, "o", ZG_MODEL_DATASET_FIELDS);

    zg_client_call_async("zfs.list", zg_args_to_json(args), on_datasets_list, page);
}

/* ------------------------------------------------------------------------- */
/* 操作                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_action_done(gboolean success,
               gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)success;

    datasets_refresh(page);
}

static void
on_rename_dataset(GtkButton *button,
                  gpointer user_data)
{
    DatasetsPage *page = user_data;
    gchar *name = selected_dataset(page);

    (void)button;

    if (name == NULL)
    {
        zg_dialogs_error(page_window(page), "未选择数据集", "请先选择要重命名的数据集。");
        return;
    }

    zg_dialogs_generic_op(page_window(page), "zfs.rename", name, NULL, NULL);

    g_free(name);
}

static void
run_mount_action(DatasetsPage *page,
                 const gchar *op_id)
{
    gchar *name = selected_dataset(page);
    ZgArgs *args;

    if (name == NULL)
    {
        zg_dialogs_error(page_window(page), "未选择数据集", "请先选择一个数据集。");
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, name);

    zg_dialogs_run_op(page_window(page), op_id, args, NULL, on_action_done, page);

    g_free(name);
}

static void
on_mount_dataset(GtkButton *button,
                 gpointer user_data)
{
    (void)button;

    run_mount_action(user_data, "zfs.mount");
}

static void
on_unmount_dataset(GtkButton *button,
                   gpointer user_data)
{
    (void)button;

    run_mount_action(user_data, "zfs.unmount");
}

/* ------------------------------------------------------------------------- */
/* 属性编辑                                                                   */
/* ------------------------------------------------------------------------- */
static void
on_edit_property(GtkButton *button,
                 gpointer user_data)
{
    DatasetsPage *page = user_data;
    ZgRow *row;
    gchar *dataset = selected_dataset(page);
    const gchar *property;
    const gchar *value;
    const gchar *source;

    (void)button;

    if (dataset == NULL)
    {
        zg_dialogs_error(page_window(page), "未选择数据集", "请先选择一个数据集。");
        return;
    }

    row = zg_table_get_selected(page->props_table);

    if (row == NULL)
    {
        zg_dialogs_error(page_window(page), "未选择属性",
                         "请先在属性列表中选中一行。");
        g_free(dataset);
        return;
    }

    property = zg_row_get_text(row, DPROP_COL_NAME);
    value = zg_row_get_text(row, DPROP_COL_VALUE);
    source = zg_row_get_text(row, DPROP_COL_SOURCE);

    zg_dialogs_property_editor(page_window(page), dataset, FALSE,
                               property, value, source);

    g_free(dataset);
}
static void
on_props_row_activated(GtkColumnView *view,
                       guint position,
                       gpointer user_data)
{
    (void)view;
    (void)position;

    on_edit_property(NULL, user_data);
}

static void
on_refresh_props(GtkButton *button,
                 gpointer user_data)
{
    DatasetsPage *page = user_data;
    gchar *dataset = selected_dataset(page);

    (void)button;

    if (dataset != NULL)
        datasets_load_detail(page, dataset);

    g_free(dataset);
}

/* ------------------------------------------------------------------------- */
/* 右键菜单                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 构建界面                                                                   */
/* ------------------------------------------------------------------------- */

static GtkWidget *
build_props_tab(DatasetsPage *page)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scrolled = gtk_scrolled_window_new();

    page->props_store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"属性", DPROP_COL_NAME, FALSE, 18, 0.0f, TRUE, FALSE},
                {"值", DPROP_COL_VALUE, TRUE, 24, 0.0f, FALSE, FALSE},
                {"来源", DPROP_COL_SOURCE, FALSE, 10, 0.0f, FALSE, TRUE},
        };

        page->props_table = zg_table_new_flat(page->props_store, columns,
                                              G_N_ELEMENTS(columns));
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->props_table);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_margin_top(scrolled, 10);
    gtk_widget_set_margin_start(scrolled, 10);
    gtk_widget_set_margin_end(scrolled, 10);
    gtk_box_append(GTK_BOX(box), scrolled);

    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_halign(row, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(row),
                       zg_ui_button("刷新", G_CALLBACK(on_refresh_props), page));
        gtk_box_append(GTK_BOX(row),
                       zg_ui_button("编辑属性", G_CALLBACK(on_edit_property), page));
        gtk_box_append(GTK_BOX(box), row);
    }

    g_signal_connect(page->props_table, "activate",
                     G_CALLBACK(on_props_row_activated), page);

    return box;
}

/* ------------------------------------------------------------------------- */
/* 对象操作面板                                                               */
/* ------------------------------------------------------------------------- */

static void
on_ds_create(GtkButton *button,
             gpointer user_data)
{
    DatasetsPage *page = user_data;
    gchar *parent = selected_dataset(page);

    (void)button;

    zg_dialogs_create_dataset(page_window(page), parent, on_action_done, page);
    g_free(parent);
}

static void
on_ds_show_props(GtkButton *button,
                 gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    gtk_notebook_set_current_page(GTK_NOTEBOOK(page->notebook), 0);
}

static void
on_ds_refresh(GtkButton *button,
              gpointer user_data)
{
    (void)button;

    zg_app_refresh_all(zg_app_get_default());
}

static void
on_ds_quota(GtkButton *button,
            gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_quota(page_window(page), page->sel_name);
}

static void
on_ds_encrypt(GtkButton *button,
              gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_encryption(page_window(page), page->sel_name, NULL);
}

static void
on_ds_delegate(GtkButton *button,
               gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_delegation(page_window(page), page->sel_name);
}

static void
on_ds_share(GtkButton *button,
            gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_share(page_window(page), page->sel_name);
}

static void
on_ds_snapshot(GtkButton *button,
               gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_create_snapshot(page_window(page), page->sel_name,
                                   on_action_done, page);
}

static void
on_ds_destroy(GtkButton *button,
              gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)button;

    if (page->sel_name != NULL)
        zg_dialogs_destroy_target(page_window(page), page->sel_name, FALSE,
                                  on_action_done, page);
}

static void
datasets_apply_usage(DatasetsPage *page)
{
    ZgDataset *dataset = zg_model_dataset_find(page->datasets, page->sel_name);
    gchar *used;
    gchar *avail;
    gchar *label;

    if (dataset == NULL)
    {
        zg_action_panel_set_usage(page->panel, -1.0, NULL);
        return;
    }

    used = zg_format_bytes(dataset->used);
    avail = zg_format_bytes(dataset->avail);
    label = g_strdup_printf("已用 %s · 可用 %s", used, avail);

    zg_action_panel_set_usage(page->panel,
                              (dataset->used + dataset->avail) > 0
                                      ? (gdouble)dataset->used /
                                                (gdouble)(dataset->used + dataset->avail)
                                      : 0.0,
                              label);

    g_free(used);
    g_free(avail);
    g_free(label);
}

static void
datasets_update_panel(DatasetsPage *page)
{
    const gchar *hint =
            "点击左侧数据集查看操作 · Enter 属性 · F2 重命名 · Delete 销毁 · F5 刷新";

    if (page->sel_name == NULL)
    {
        ZgAction actions[] = {
                {"刷新", "view-refresh-symbolic", "重新读取列表（F5）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_ds_refresh), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "folder-symbolic",
                                           .title = "数据集",
                                           .subtitle = "选择一个数据集开始操作",
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = "提示：在左侧点一个对象，再用上方按钮操作；创建/导入见上方工具栏"});
        return;
    }

    {
        ZgAction actions[10];
        guint n = 0;

        actions[n++] = (ZgAction){
                "属性", "document-properties-symbolic", "查看与修改属性（Enter）",
                FALSE, TRUE, FALSE, G_CALLBACK(on_ds_show_props), page};
        actions[n++] = (ZgAction){
                "创建快照", "camera-photo-symbolic", "为这个数据集创建快照",
                FALSE, FALSE, FALSE, G_CALLBACK(on_ds_snapshot), page};

        if (page->sel_mounted)
            actions[n++] = (ZgAction){
                    "卸载", "media-playback-stop-symbolic", "卸载数据集",
                    FALSE, FALSE, FALSE, G_CALLBACK(on_unmount_dataset), page};
        else
            actions[n++] = (ZgAction){
                    "挂载", "media-playback-start-symbolic", "挂载数据集",
                    FALSE, FALSE, FALSE, G_CALLBACK(on_mount_dataset), page};

        actions[n++] = (ZgAction){
                "重命名", "document-edit-symbolic", "重命名数据集（F2）",
                FALSE, FALSE, FALSE, G_CALLBACK(on_rename_dataset), page};
        actions[n++] = (ZgAction){
                "配额", "drive-harddisk-symbolic", "设置用户 / 组配额",
                FALSE, FALSE, FALSE, G_CALLBACK(on_ds_quota), page};
        actions[n++] = (ZgAction){
                "加密", "channel-secure-symbolic", "加载 / 卸载 / 更换密钥",
                FALSE, FALSE, FALSE, G_CALLBACK(on_ds_encrypt), page};
        actions[n++] = (ZgAction){
                "委派", "system-users-symbolic", "把权限委派给普通用户",
                FALSE, FALSE, FALSE, G_CALLBACK(on_ds_delegate), page};
        actions[n++] = (ZgAction){
                "共享", "network-transmit-receive-symbolic", "通过 NFS / SMB 共享",
                FALSE, FALSE, FALSE, G_CALLBACK(on_ds_share), page};
        actions[n++] = (ZgAction){
                "销毁", "edit-delete-symbolic", "永久删除数据集（Delete）",
                TRUE, FALSE, TRUE, G_CALLBACK(on_ds_destroy), page};

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = g_strcmp0(page->sel_type, "volume") == 0
                                                           ? "drive-harddisk-symbolic"
                                                           : "folder-symbolic",
                                           .title = page->sel_name,
                                           .subtitle = g_strcmp0(page->sel_type, "volume") == 0
                                                               ? "卷 (zvol)"
                                                               : "文件系统",
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = n,
                                           .hint = hint});
        datasets_apply_usage(page);
    }
}

static void
on_table_activated(GtkColumnView *view,
                   guint position,
                   gpointer user_data)
{
    (void)view;
    (void)position;

    on_ds_show_props(NULL, user_data);
}

static gboolean
on_dataset_key(GtkEventControllerKey *controller,
               guint keyval,
               guint keycode,
               GdkModifierType state,
               gpointer user_data)
{
    DatasetsPage *page = user_data;

    (void)controller;
    (void)keycode;
    (void)state;

    switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        on_ds_show_props(NULL, page);
        return TRUE;
    case GDK_KEY_F2:
        on_rename_dataset(NULL, page);
        return TRUE;
    case GDK_KEY_Delete:
        on_ds_destroy(NULL, page);
        return TRUE;
    case GDK_KEY_F5:
        zg_app_refresh_all(page->app);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------------- */
/* 实时容量刷新（递归更新子节点文本）                                         */
/* ------------------------------------------------------------------------- */

static void
datasets_live_row(ZgRow *row,
                  GPtrArray *datasets)
{
    DsTag *tag = zg_row_get_tag(row);
    GListStore *children;

    if (tag != NULL)
    {
        ZgDataset *dataset = zg_model_dataset_find(datasets, tag->fullname);

        if (dataset != NULL)
        {
            gchar *used = zg_format_bytes(dataset->used);
            gchar *avail = zg_format_bytes(dataset->avail);
            gchar *refer = zg_format_bytes(dataset->refer);

            zg_row_set_text(row, DS_COL_USED, used);
            zg_row_set_text(row, DS_COL_AVAIL, avail);
            zg_row_set_text(row, DS_COL_REFER, refer);

            g_free(used);
            g_free(avail);
            g_free(refer);
        }
    }

    children = zg_row_get_children(row);

    if (children != NULL)
    {
        guint i;
        guint n = g_list_model_get_n_items(G_LIST_MODEL(children));

        for (i = 0; i < n; i++)
        {
            ZgRow *child = ZG_ROW(g_list_model_get_item(G_LIST_MODEL(children), i));

            datasets_live_row(child, datasets);
            g_object_unref(child);
        }
    }
}

static void
on_datasets_live(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DatasetsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GListModel *model;
    guint i;
    guint n;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_client_result_free(response);
        return;
    }

    if (page->datasets != NULL)
        g_ptr_array_free(page->datasets, TRUE);

    page->datasets = zg_model_datasets_from_tsv(response->stdout_text);

    model = G_LIST_MODEL(page->store);
    n = g_list_model_get_n_items(model);

    for (i = 0; i < n; i++)
    {
        ZgRow *row = ZG_ROW(g_list_model_get_item(model, i));

        datasets_live_row(row, page->datasets);
        g_object_unref(row);
    }

    datasets_apply_usage(page);

    zg_client_result_free(response);
}

static void
datasets_live_refresh(gpointer data)
{
    DatasetsPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_option(args, "o", ZG_MODEL_DATASET_FIELDS);

    zg_client_call_async("zfs.list", zg_args_to_json(args), on_datasets_live, page);
}

GtkWidget *
zg_page_datasets_new(ZgApp *app)
{
    DatasetsPage *page = g_new0(DatasetsPage, 1);
    GtkWidget *paned;
    GtkWidget *left;
    GtkWidget *toolbar;
    GtkWidget *scrolled;
    GtkWidget *notebook;
    GtkWidget *userspace_text;
    GtkWidget *groupspace_text;
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
                      zg_ui_button("创建数据集", G_CALLBACK(on_ds_create), page));
    gtk_box_append(GTK_BOX(left), toolbar);

    page->store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"数据集", DS_COL_NAME, TRUE, 16, 0.0f, TRUE, FALSE},
                {"类型", DS_COL_TYPE, FALSE, 8, 0.0f, FALSE, FALSE},
                {"已用", DS_COL_USED, FALSE, 8, 1.0f, FALSE, FALSE},
                {"可用", DS_COL_AVAIL, FALSE, 8, 1.0f, FALSE, FALSE},
                {"引用", DS_COL_REFER, FALSE, 8, 1.0f, FALSE, FALSE},
                {"挂载点", DS_COL_MOUNT, TRUE, 14, 0.0f, FALSE, FALSE},
                {"创建时间", DS_COL_CREATED, FALSE, 18, 0.0f, FALSE, TRUE},
        };

        page->table = zg_table_new_tree(page->store, columns,
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

        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_dataset_key), page);
        gtk_widget_add_controller(page->table, keys);
    }

    notebook = gtk_notebook_new();
    page->notebook = notebook;
    gtk_paned_set_end_child(GTK_PANED(paned), notebook);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_props_tab(page),
                             gtk_label_new("属性"));

    userspace_text = zg_ui_text_view_new(&page->userspace_buffer, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), userspace_text,
                             gtk_label_new("用户配额"));

    groupspace_text = zg_ui_text_view_new(&page->groupspace_buffer, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), groupspace_text,
                             gtk_label_new("组配额"));

    gtk_paned_set_position(GTK_PANED(paned), 820);

    g_object_set_data(G_OBJECT(page->root), "zg-notebook", notebook);
    g_object_set_data_full(G_OBJECT(page->root), "zg-page-datasets", page, g_free);

    zg_app_register_refresh(app, datasets_refresh, page);
    zg_app_register_live_refresh(app, datasets_live_refresh, page);
    datasets_update_panel(page);

    return page->root;
}
