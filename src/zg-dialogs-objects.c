/* zg-dialogs-objects.c - 面向对象的操作对话框
 *
 * 与 zg-dialogs.c 的区别：这些对话框只呈现「对象 + 语义选项」，
 * 例如"勾选磁盘 → 选择镜像/RAID-Z → 添加"，不暴露任何命令行参数。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"

/* 对话框尺寸常量 */
#define ZG_DIALOG_WIDTH 640
#define ZG_CHOICE_BASE_HEIGHT 300
#define ZG_CHOICE_ROW_HEIGHT 48
#define ZG_INPUT_HEIGHT 360
#include "zg-model.h"
#include "zg-ui.h"
#include "zg-util.h"

/* ------------------------------------------------------------------------- */
/* 通用「确认 + 选项」                                                        */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget **checks;
    guint count;
    ZgChoiceFunc on_accept;
    gpointer data;
} ChoiceDialog;

static void
choice_dialog_free(gpointer data)
{
    ChoiceDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->checks);
    g_free(dialog);
}

static void
on_choice_accept(GtkButton *button,
                 gpointer user_data)
{
    ChoiceDialog *dialog = user_data;
    ZgChoiceFunc callback = dialog->on_accept;
    gpointer data = dialog->data;
    gboolean *selected;
    guint i;

    (void)button;

    selected = g_new0(gboolean, dialog->count + 1);

    for (i = 0; i < dialog->count; i++)
        selected[i] = gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->checks[i]));

    gtk_window_destroy(GTK_WINDOW(dialog->window));

    callback(selected, data);

    g_free(selected);
}

void zg_dialogs_choice(GtkWindow *parent,
                       const ZgChoiceSpec *spec,
                       ZgChoiceFunc on_accept,
                       gpointer data)
{
    ChoiceDialog *dialog = g_new0(ChoiceDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *accept_button;
    guint i;

    window = zg_dialogs_window_new(parent, spec->title, ZG_DIALOG_WIDTH,
                                   ZG_CHOICE_BASE_HEIGHT +
                                           (gint)spec->n_choices * ZG_CHOICE_ROW_HEIGHT);
    dialog->window = GTK_WIDGET(window);
    dialog->on_accept = on_accept;
    dialog->data = data;
    dialog->count = spec->n_choices;
    dialog->checks = g_new0(GtkWidget *, spec->n_choices + 1);

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading(spec->heading));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(spec->description));

    for (i = 0; i < spec->n_choices; i++)
    {
        GtkWidget *check = gtk_check_button_new_with_label(spec->choices[i].label);

        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), spec->choices[i].active);
        gtk_widget_set_margin_start(check, 4);
        gtk_box_append(GTK_BOX(body), check);

        if (spec->choices[i].hint != NULL && spec->choices[i].hint[0] != '\0')
        {
            GtkWidget *hint = zg_ui_label_dim(spec->choices[i].hint);

            gtk_widget_set_margin_start(hint, 28);
            gtk_box_append(GTK_BOX(body), hint);
        }

        dialog->checks[i] = check;
    }

    zg_dialogs_button_row(window, NULL, &accept_button, spec->accept_label, spec->destructive);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_choice_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-choice", dialog,
                           choice_dialog_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 磁盘选择                                                                   */
/* ------------------------------------------------------------------------- */

static gboolean
disk_name_is_used(const gchar *const *used,
                  const gchar *name)
{
    guint i;

    if (used == NULL || name == NULL)
        return FALSE;

    for (i = 0; used[i] != NULL; i++)
    {
        if (used[i][0] != '\0' && strstr(used[i], name) != NULL)
            return TRUE;
    }

    return FALSE;
}

typedef struct
{
    GtkWindow *parent;
    gchar *pool;
    gchar *vdev;
    gchar **used;
    ZgDialogDoneFunc done;
    gpointer data;
} DiskRequest;

static void
disk_request_free(gpointer data)
{
    DiskRequest *request = data;

    if (request == NULL)
        return;

    g_free(request->pool);
    g_free(request->vdev);
    g_strfreev(request->used);
    g_free(request);
}

/* ------------------------------------------------------------------------- */
/* 添加磁盘                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *purpose_drop;
    GtkWidget *topology_drop;
    GtkWidget *disk_box;
    GtkWidget *dryrun_check;
    gchar *pool;
    ZgDialogDoneFunc done;
    gpointer data;
} AddVdevDialog;

static void
add_vdev_dialog_free(gpointer data)
{
    AddVdevDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->pool);
    g_free(dialog);
}

static GPtrArray *
checked_disks(GtkWidget *disk_box)
{
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GtkWidget *child = gtk_widget_get_first_child(disk_box);

    while (child != NULL)
    {
        if (GTK_IS_CHECK_BUTTON(child) &&
            gtk_check_button_get_active(GTK_CHECK_BUTTON(child)))
        {
            const gchar *name = g_object_get_data(G_OBJECT(child), "zg-disk-name");

            if (name != NULL)
                g_ptr_array_add(names, g_strdup(name));
        }

        child = gtk_widget_get_next_sibling(child);
    }

    return names;
}

static void
on_add_vdev_accept(GtkButton *button,
                   gpointer user_data)
{
    AddVdevDialog *dialog = user_data;
    const guint purpose = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->purpose_drop));
    const guint topology = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->topology_drop));
    GPtrArray *disks = checked_disks(dialog->disk_box);
    static const gchar *const purposes[] = {NULL, "log", "cache", "spare",
                                            "special", "dedup"};
    static const gchar *const topologies[] = {NULL, "mirror", "raidz1",
                                              "raidz2", "raidz3", NULL};
    guint required = 1;
    guint i;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (disks->len == 0)
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请选择磁盘",
                         "至少勾选一块要添加的磁盘。");
        g_ptr_array_free(disks, TRUE);
        return;
    }

    if (purpose == 0)
    {
        switch (topology)
        {
        case 1:
            required = 2;
            break;
        case 2:
            required = 2;
            break;
        case 3:
            required = 3;
            break;
        case 4:
            required = 4;
            break;
        case 5:
            required = 2;
            break;
        default:
            required = 1;
            break;
        }
    }

    if (disks->len < required)
    {
        gchar *detail = g_strdup_printf("所选布局至少需要 %u 块磁盘，当前勾选了 %u 块。",
                                        required, disks->len);

        zg_dialogs_error(GTK_WINDOW(dialog->window), "磁盘数量不足", detail);
        g_free(detail);
        g_ptr_array_free(disks, TRUE);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, dialog->pool);

    if (purpose != 0)
        zg_args_add_positional(args, purposes[purpose]);

    if (purpose == 0 && topologies[topology] != NULL)
        zg_args_add_positional(args, topologies[topology]);

    for (i = 0; i < disks->len; i++)
    {
        gchar *device = g_strdup_printf("/dev/%s",
                                        (gchar *)g_ptr_array_index(disks, i));

        zg_args_add_positional(args, device);
        g_free(device);
    }

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->dryrun_check)))
        zg_args_add_flag(args, "-n");

    message = g_strdup_printf("已向存储池 %s 添加 %u 块磁盘。",
                              dialog->pool, disks->len);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->data;

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          "zpool.add", args, message, done, done_data);
    }

    g_free(message);
    g_ptr_array_free(disks, TRUE);
}

static void
on_disks_for_add(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DiskRequest *request = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GPtrArray *disks;
    AddVdevDialog *dialog;
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *scrolled;
    GtkWidget *accept_button;
    gint row = 0;
    guint i;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_dialogs_error(request->parent, "无法读取磁盘列表",
                         response != NULL ? response->message : "后台服务无应答");
        zg_client_result_free(response);
        disk_request_free(request);
        return;
    }

    disks = zg_model_disks_from_text(response->stdout_text);
    zg_client_result_free(response);

    if (disks->len == 0)
    {
        zg_dialogs_info(request->parent, "没有可用磁盘",
                        "系统未报告任何磁盘，请检查 geom(8) 是否可用。");
        g_ptr_array_free(disks, TRUE);
        disk_request_free(request);
        return;
    }

    dialog = g_new0(AddVdevDialog, 1);
    dialog->pool = g_strdup(request->pool);
    dialog->done = request->done;
    dialog->data = request->data;

    window = zg_dialogs_window_new(request->parent, "添加磁盘到存储池", 740, 640);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("向存储池添加磁盘"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(
                                          "勾选要加入的磁盘，选择用途与布局。界面会自动组合出正确的命令，"
                                          "你不需要填写任何参数。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 6);
    gtk_box_append(GTK_BOX(body), grid);

    {
        static const gchar *const purposes[] = {
                "数据（参与存储）", "日志 log", "缓存 cache", "热备 spare",
                "特殊 special", "去重 dedup", NULL};
        static const gchar *const topologies[] = {
                "单盘", "镜像 mirror", "RAID-Z1", "RAID-Z2", "RAID-Z3",
                "条带（无冗余）", NULL};

        dialog->purpose_drop = gtk_drop_down_new_from_strings(purposes);
        zg_dialogs_add_field(grid, &row, "用途", dialog->purpose_drop,
                             "日志/缓存类设备不参与数据冗余");

        dialog->topology_drop = gtk_drop_down_new_from_strings(topologies);
        zg_dialogs_add_field(grid, &row, "布局", dialog->topology_drop,
                             "镜像与 RAID-Z 需要多块磁盘");
    }

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("可用磁盘"));

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    dialog->disk_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dialog->disk_box);
    gtk_box_append(GTK_BOX(body), scrolled);

    for (i = 0; i < disks->len; i++)
    {
        ZgDisk *disk = g_ptr_array_index(disks, i);
        gboolean used = disk_name_is_used((const gchar *const *)request->used,
                                          disk->name);
        gchar *size_text = zg_format_bytes(disk->size);
        gchar *label;
        GtkWidget *check;

        label = g_strdup_printf("/dev/%s — %s（%s）%s",
                                disk->name,
                                disk->descr != NULL && disk->descr[0] != '\0'
                                        ? disk->descr
                                        : "磁盘",
                                size_text,
                                used ? " · 已被占用" : "");

        check = gtk_check_button_new_with_label(label);
        gtk_widget_set_sensitive(check, !used);
        g_object_set_data_full(G_OBJECT(check), "zg-disk-name",
                               g_strdup(disk->name), g_free);
        gtk_box_append(GTK_BOX(dialog->disk_box), check);

        g_free(label);
        g_free(size_text);
    }

    dialog->dryrun_check = gtk_check_button_new_with_label("仅演练，不实际添加");
    gtk_box_append(GTK_BOX(body), dialog->dryrun_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "添加磁盘", TRUE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_add_vdev_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-add-vdev", dialog,
                           add_vdev_dialog_free);

    gtk_window_present(window);

    g_ptr_array_free(disks, TRUE);
    disk_request_free(request);
}

void zg_dialogs_add_vdev(GtkWindow *parent,
                         const gchar *pool,
                         const gchar *const *used_devices,
                         ZgDialogDoneFunc done,
                         gpointer data)
{
    DiskRequest *request = g_new0(DiskRequest, 1);

    request->parent = parent;
    request->pool = g_strdup(pool);
    request->used = g_strdupv((gchar **)used_devices);
    request->done = done;
    request->data = data;

    zg_client_call_async("system.disks", NULL, on_disks_for_add, request);
}

/* ------------------------------------------------------------------------- */
/* 更换磁盘                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *list;
    gchar *pool;
    gchar *vdev;
    ZgDialogDoneFunc done;
    gpointer data;
} ReplaceDialog;

static void
replace_dialog_free(gpointer data)
{
    ReplaceDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->pool);
    g_free(dialog->vdev);
    g_free(dialog);
}

static void
on_replace_accept(GtkButton *button,
                  gpointer user_data)
{
    ReplaceDialog *dialog = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(dialog->list));
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (row == NULL)
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请选择新磁盘",
                         "请在列表中选择用于替换的磁盘。");
        return;
    }

    {
        const gchar *disk = g_object_get_data(G_OBJECT(row), "zg-disk-name");
        gchar *device = g_strdup_printf("/dev/%s", disk);

        args = zg_args_new();
        zg_args_add_positional(args, dialog->pool);
        zg_args_add_positional(args, dialog->vdev);
        zg_args_add_positional(args, device);

        message = g_strdup_printf("正在用 %s 替换 %s，替换会在后台重建数据。",
                                  device, dialog->vdev);

        {
            ZgDialogDoneFunc done = dialog->done;
            gpointer done_data = dialog->data;

            gtk_window_destroy(GTK_WINDOW(dialog->window));
            zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                              "zpool.replace", args, message, done, done_data);
        }

        g_free(message);
        g_free(device);
    }
}

static void
on_disks_for_replace(GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
    DiskRequest *request = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GPtrArray *disks;
    ReplaceDialog *dialog;
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *scrolled;
    GtkWidget *accept_button;
    guint i;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_dialogs_error(request->parent, "无法读取磁盘列表",
                         response != NULL ? response->message : "后台服务无应答");
        zg_client_result_free(response);
        disk_request_free(request);
        return;
    }

    disks = zg_model_disks_from_text(response->stdout_text);
    zg_client_result_free(response);

    dialog = g_new0(ReplaceDialog, 1);
    dialog->pool = g_strdup(request->pool);
    dialog->vdev = g_strdup(request->vdev);
    dialog->done = request->done;
    dialog->data = request->data;

    window = zg_dialogs_window_new(request->parent, "更换磁盘", 720, 560);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("选择新磁盘"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(
                                          "旧磁盘上的数据会从其他副本重建到新磁盘。请确认新磁盘中没有重要数据。"));

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    dialog->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dialog->list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dialog->list);
    gtk_box_append(GTK_BOX(body), scrolled);

    for (i = 0; i < disks->len; i++)
    {
        ZgDisk *disk = g_ptr_array_index(disks, i);
        gboolean used = disk_name_is_used((const gchar *const *)request->used,
                                          disk->name);
        gchar *size_text = zg_format_bytes(disk->size);
        gchar *label;
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);

        label = g_strdup_printf("/dev/%s%s", disk->name,
                                used ? " · 已被占用" : "");
        gtk_box_append(GTK_BOX(box), zg_ui_label_heading(label));
        gtk_box_append(GTK_BOX(box), zg_ui_label_dim(
                                             disk->descr != NULL ? disk->descr : "磁盘"));
        gtk_box_append(GTK_BOX(box), zg_ui_label_dim(size_text));

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), !used);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), !used);
        g_object_set_data_full(G_OBJECT(row), "zg-disk-name",
                               g_strdup(disk->name), g_free);
        gtk_list_box_append(GTK_LIST_BOX(dialog->list), row);

        g_free(label);
        g_free(size_text);
    }

    zg_dialogs_button_row(window, NULL, &accept_button, "开始替换", TRUE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_replace_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-replace", dialog,
                           replace_dialog_free);

    gtk_window_present(window);

    g_ptr_array_free(disks, TRUE);
    disk_request_free(request);
}

void zg_dialogs_replace_disk(GtkWindow *parent,
                             const gchar *pool,
                             const gchar *vdev,
                             const gchar *const *used_devices,
                             ZgDialogDoneFunc done,
                             gpointer data)
{
    DiskRequest *request = g_new0(DiskRequest, 1);

    request->parent = parent;
    request->pool = g_strdup(pool);
    request->vdev = g_strdup(vdev);
    request->used = g_strdupv((gchar **)used_devices);
    request->done = done;
    request->data = data;

    zg_client_call_async("system.disks", NULL, on_disks_for_replace, request);
}

/* ------------------------------------------------------------------------- */
/* 单字段输入                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *entry;
    ZgInputFunc on_accept;
    gpointer data;
} InputDialog;

static void
input_dialog_free(gpointer data)
{
    g_free(data);
}

static void
on_input_accept(GtkButton *button,
                gpointer user_data)
{
    InputDialog *dialog = user_data;
    gchar *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(dialog->entry)));
    ZgInputFunc callback = dialog->on_accept;
    gpointer data = dialog->data;

    (void)button;

    gtk_window_destroy(GTK_WINDOW(dialog->window));

    callback(text, data);

    g_free(text);
}

void zg_dialogs_input(GtkWindow *parent,
                      const ZgInputSpec *spec,
                      ZgInputFunc on_accept,
                      gpointer data)
{
    InputDialog *dialog = g_new0(InputDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gint grid_row = 0;

    dialog->on_accept = on_accept;
    dialog->data = data;

    window = zg_dialogs_window_new(parent, spec->title, ZG_DIALOG_WIDTH, ZG_INPUT_HEIGHT);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading(spec->heading));

    if (spec->description != NULL)
        gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(spec->description));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->entry),
                                   spec->placeholder != NULL ? spec->placeholder : "");
    gtk_editable_set_text(GTK_EDITABLE(dialog->entry),
                          spec->default_value != NULL ? spec->default_value : "");
    gtk_editable_set_position(GTK_EDITABLE(dialog->entry), -1);

    zg_dialogs_add_field(grid, &grid_row,
                         spec->field_label != NULL ? spec->field_label : "名称",
                         dialog->entry, NULL);

    zg_dialogs_button_row(window, NULL, &accept_button, spec->accept_label, spec->destructive);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_input_accept), dialog);
    g_signal_connect(dialog->entry, "activate", G_CALLBACK(on_input_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-input", dialog,
                           input_dialog_free);

    gtk_window_present(window);
    gtk_widget_grab_focus(dialog->entry);
}
