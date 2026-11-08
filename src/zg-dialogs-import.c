/* zg-dialogs-import.c - 导入存储池浏览器
 *
 * 扫描当前系统（或指定目录）中可导入的池，用户勾选/点选即可导入，
 * 不需要了解 zpool import 的任何参数。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-model.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    GtkWidget *window;
    GtkWidget *list;
    GtkWidget *directory_entry;
    GtkWidget *status_label;
    GtkWidget *scan_button;
    GtkWidget *force_check;
    GtkWidget *nomount_check;
    GtkWindow *parent;
    ZgDialogDoneFunc done;
    gpointer data;
} ImportDialog;

typedef struct
{
    ImportDialog *dialog;
    gchar *directory;
} ImportScan;

static void
import_scan_free(gpointer data)
{
    ImportScan *scan = data;

    if (scan == NULL)
        return;

    g_free(scan->directory);
    g_free(scan);
}

static void
on_import_accept(GtkButton *button,
                 gpointer user_data)
{
    ImportDialog *dialog = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(dialog->list));
    const gchar *pool;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (row == NULL)
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请选择存储池",
                         "请在上方列表中选择要导入的池。");
        return;
    }

    pool = g_object_get_data(G_OBJECT(row), "zg-pool-name");
    args = zg_args_new();
    zg_args_add_positional(args, pool);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->force_check)))
        zg_args_add_flag(args, "-f");

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->nomount_check)))
        zg_args_add_flag(args, "-N");

    message = g_strdup_printf("存储池 %s 已导入。", pool);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->data;

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          "zpool.import", args, message, done, done_data);
    }

    g_free(message);
}

static void
import_populate(ImportDialog *dialog,
                ZgClientResult *response)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(dialog->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(dialog->list), child);

    if (response == NULL || !response->ok)
    {
        gtk_label_set_text(GTK_LABEL(dialog->status_label),
                           response != NULL ? response->message
                                            : "后台服务无应答");
        return;
    }

    {
        GPtrArray *pools = zg_model_importable_from_text(response->stdout_text);
        guint i;

        for (i = 0; i < pools->len; i++)
        {
            ZgImportablePool *pool = g_ptr_array_index(pools, i);
            GtkWidget *row = gtk_list_box_row_new();
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget *title = gtk_label_new(NULL);
            gchar *markup;

            gtk_widget_set_margin_top(box, 6);
            gtk_widget_set_margin_bottom(box, 6);
            gtk_widget_set_margin_start(box, 10);
            gtk_widget_set_margin_end(box, 10);

            markup = g_markup_printf_escaped("<b>%s</b>   %s",
                                             pool->pool,
                                             pool->state != NULL ? pool->state : "");
            gtk_label_set_markup(GTK_LABEL(title), markup);
            gtk_label_set_xalign(GTK_LABEL(title), 0.0);
            g_free(markup);

            gtk_box_append(GTK_BOX(box), title);

            if (pool->note != NULL && pool->note[0] != '\0')
                gtk_box_append(GTK_BOX(box), zg_ui_label_dim(pool->note));

            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
            g_object_set_data_full(G_OBJECT(row), "zg-pool-name",
                                   g_strdup(pool->pool), g_free);
            gtk_list_box_append(GTK_LIST_BOX(dialog->list), row);
        }

        if (pools->len == 0)
            gtk_label_set_text(GTK_LABEL(dialog->status_label),
                               "没有发现可导入的存储池。可尝试指定设备目录后重新扫描。");
        else
        {
            gchar *text = g_strdup_printf("发现 %u 个可导入的存储池，选中后点击「导入」。",
                                          pools->len);

            gtk_label_set_text(GTK_LABEL(dialog->status_label), text);
            g_free(text);
        }

        g_ptr_array_free(pools, TRUE);
    }
}

static void
on_import_list(GObject *source,
               GAsyncResult *result,
               gpointer user_data)
{
    ImportScan *scan = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    import_populate(scan->dialog, response);

    gtk_widget_set_sensitive(scan->dialog->scan_button, TRUE);

    zg_client_result_free(response);
    import_scan_free(scan);
}

static void
import_refresh(ImportDialog *dialog)
{
    const gchar *directory = gtk_editable_get_text(GTK_EDITABLE(dialog->directory_entry));
    ZgArgs *args = zg_args_new();
    ImportScan *scan;

    if (directory != NULL && directory[0] != '\0')
        zg_args_add_option(args, "d", directory);

    gtk_label_set_text(GTK_LABEL(dialog->status_label), "正在扫描…");
    gtk_widget_set_sensitive(dialog->scan_button, FALSE);

    scan = g_new0(ImportScan, 1);
    scan->dialog = dialog;
    scan->directory = g_strdup(directory);

    zg_client_call_async("zpool.import", zg_args_to_json(args),
                         on_import_list, scan);
}

static void
on_format_disk_clicked(GtkButton *button,
                       gpointer user_data)
{
    ImportDialog *dialog = user_data;

    (void)button;

    /* 打开“创建存储池”向导：选择设备即会写入 ZFS 标签（清空原文件系统） */
    zg_dialogs_create_pool(GTK_WINDOW(dialog->window), NULL, NULL);
}

static void
on_import_scan_clicked(GtkButton *button,
                       gpointer user_data)
{
    (void)button;

    import_refresh(user_data);
}

static void
import_dialog_free(gpointer data)
{
    g_free(data);
}

void zg_dialogs_import_pool(GtkWindow *parent,
                            ZgDialogDoneFunc done,
                            gpointer data)
{
    ImportDialog *dialog = g_new0(ImportDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *scrolled;
    GtkWidget *scan_row;
    GtkWidget *accept_button;

    dialog->parent = parent;
    dialog->done = done;
    dialog->data = data;

    window = zg_dialogs_window_new(parent, "导入存储池", 780, 640);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("可导入的存储池"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(
                                          "选择要导入的存储池。导入后其中的数据集会按原挂载点恢复；"
                                          "如果磁盘来自另一台机器，通常需要勾选「强制导入」。"));

    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(
                                          "外置硬盘：已有 ZFS 池可直接导入；若是 NTFS / ext4 / "
                                          "exFAT / APFS 等其他文件系统，ZFS 无法直接读取，需要先"
                                          "格式化为 ZFS（会清空该磁盘上的数据）。"));

    {
        GtkWidget *format_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *format_button = zg_ui_button("格式化磁盘为 ZFS（新建池）",
                                                G_CALLBACK(on_format_disk_clicked),
                                                dialog);

        gtk_widget_add_css_class(format_button, "suggested-action");
        gtk_box_append(GTK_BOX(format_row), format_button);
        gtk_box_append(GTK_BOX(body), format_row);
    }

    scan_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    dialog->directory_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->directory_entry),
                                   "扫描目录，例如 /dev（留空为默认路径）");
    gtk_widget_set_hexpand(dialog->directory_entry, TRUE);

    dialog->scan_button = zg_ui_button("重新扫描", G_CALLBACK(on_import_scan_clicked),
                                       dialog);

    gtk_box_append(GTK_BOX(scan_row), zg_ui_label_dim("设备目录"));
    gtk_box_append(GTK_BOX(scan_row), dialog->directory_entry);
    gtk_box_append(GTK_BOX(scan_row), dialog->scan_button);
    gtk_box_append(GTK_BOX(body), scan_row);

    dialog->status_label = zg_ui_label_dim("正在扫描…");
    gtk_box_append(GTK_BOX(body), dialog->status_label);

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    dialog->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(dialog->list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), dialog->list);
    gtk_box_append(GTK_BOX(body), scrolled);

    dialog->force_check = gtk_check_button_new_with_label("强制导入（忽略上次导出状态）");
    dialog->nomount_check = gtk_check_button_new_with_label("导入后不自动挂载");
    gtk_box_append(GTK_BOX(body), dialog->force_check);
    gtk_box_append(GTK_BOX(body), dialog->nomount_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "导入所选存储池", FALSE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_import_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-import", dialog,
                           import_dialog_free);

    gtk_window_present(window);

    import_refresh(dialog);
}
