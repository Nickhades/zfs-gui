/* zg-page-backup.c - 系统备份页面
 *
 * 一键把整个系统的递归快照以 zfs send 流文件写入外置盘，支持全量与增量，
 * 并管理备份文件（刷新 / 删除 / 恢复）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-pages.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "zg-app.h"
#include "zg-argv.h"
#include "zg-dialogs.h"
#include "zg-model.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GtkStringList *pools;
    GtkWidget *pool_drop;
    GtkWidget *target_entry;
    GtkWidget *state_label;
    GtkWidget *list;
    gchar *last_snapshot;
    gchar *last_kind;
    gchar *last_file;
} BackupPage;

typedef struct
{
    BackupPage *page;
    gchar *snapshot;
    gchar *file;
    gchar *kind;
    gchar *base;
} BackupJob;

typedef struct
{
    BackupPage *page;
    gchar *path;
} RestoreContext;

#define BACKUP_SNAPSHOT_PREFIX "sys-"

/* ------------------------------------------------------------------------- */
/* 状态文件                                                                   */
/* ------------------------------------------------------------------------- */

static gchar *
backup_state_path(void)
{
    gchar *dir = g_build_filename(g_get_user_config_dir(), "zfs-gui", NULL);
    gchar *path;

    g_mkdir_with_parents(dir, 0700);
    path = g_build_filename(dir, "backup.ini", NULL);
    g_free(dir);

    return path;
}

static void
backup_save_state(BackupPage *page)
{
    GKeyFile *keyfile = g_key_file_new();
    gchar *path = backup_state_path();

    g_key_file_set_string(keyfile, "backup", "target",
                          gtk_editable_get_text(GTK_EDITABLE(page->target_entry)));
    g_key_file_set_string(keyfile, "backup", "last_snapshot",
                          page->last_snapshot != NULL ? page->last_snapshot : "");
    g_key_file_set_string(keyfile, "backup", "last_kind",
                          page->last_kind != NULL ? page->last_kind : "");
    g_key_file_set_string(keyfile, "backup", "last_file",
                          page->last_file != NULL ? page->last_file : "");
    g_key_file_save_to_file(keyfile, path, NULL);

    g_key_file_unref(keyfile);
    g_free(path);
}

static void
backup_load_state(BackupPage *page)
{
    GKeyFile *keyfile = g_key_file_new();
    gchar *path = backup_state_path();
    gchar *target;
    gchar *value;

    if (!g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL))
    {
        g_key_file_unref(keyfile);
        g_free(path);
        return;
    }

    target = g_key_file_get_string(keyfile, "backup", "target", NULL);
    if (target != NULL && target[0] != '\0')
        gtk_editable_set_text(GTK_EDITABLE(page->target_entry), target);

    value = g_key_file_get_string(keyfile, "backup", "last_snapshot", NULL);
    if (value != NULL && value[0] != '\0')
        page->last_snapshot = g_strdup(value);
    g_free(value);

    value = g_key_file_get_string(keyfile, "backup", "last_kind", NULL);
    if (value != NULL && value[0] != '\0')
        page->last_kind = g_strdup(value);
    g_free(value);

    value = g_key_file_get_string(keyfile, "backup", "last_file", NULL);
    if (value != NULL && value[0] != '\0')
        page->last_file = g_strdup(value);
    g_free(value);

    g_free(target);
    g_key_file_unref(keyfile);
    g_free(path);
}

static void
backup_update_state_label(BackupPage *page)
{
    gchar *text;

    if (page->last_snapshot == NULL)
    {
        gtk_label_set_text(GTK_LABEL(page->state_label),
                           "还没有备份记录：请先执行一次「全量备份」");
        return;
    }

    text = g_strdup_printf("上次备份：%s · %s · %s",
                           page->last_snapshot,
                           page->last_kind != NULL ? page->last_kind : "",
                           page->last_file != NULL ? page->last_file : "");
    gtk_label_set_text(GTK_LABEL(page->state_label), text);
    g_free(text);
}

static gboolean
backup_target_valid(BackupPage *page)
{
    const gchar *target = gtk_editable_get_text(GTK_EDITABLE(page->target_entry));
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));

    if (target == NULL || target[0] != '/')
    {
        zg_dialogs_error(window, "备份目录无效",
                         "请填写外置盘上的绝对路径，例如 /backup，"
                         "或点右侧按钮选择目录。");
        return FALSE;
    }

    if (!g_file_test(target, G_FILE_TEST_IS_DIR))
    {
        zg_dialogs_error(window, "备份目录不存在",
                         "目标目录不存在或不是目录。请先在外置盘上创建 ZFS 池并挂载。");
        return FALSE;
    }

    return TRUE;
}

static const gchar *
backup_selected_pool(BackupPage *page)
{
    guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->pool_drop));

    if (index == GTK_INVALID_LIST_POSITION ||
        index >= g_list_model_get_n_items(G_LIST_MODEL(page->pools)))
        return NULL;

    return gtk_string_list_get_string(page->pools, index);
}

static gchar *
backup_now_stamp(void)
{
    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");

    g_date_time_unref(now);
    return stamp;
}

/* ------------------------------------------------------------------------- */
/* 备份执行                                                                   */
/* ------------------------------------------------------------------------- */

static void
backup_job_free(BackupJob *job)
{
    if (job == NULL)
        return;

    g_free(job->snapshot);
    g_free(job->file);
    g_free(job->kind);
    g_free(job->base);
    g_free(job);
}

static void backup_refresh_list(BackupPage *page);

static void
on_backup_send_done(gboolean ok,
                    gpointer data)
{
    BackupJob *job = data;

    if (ok)
    {
        g_free(job->page->last_snapshot);
        g_free(job->page->last_kind);
        g_free(job->page->last_file);
        job->page->last_snapshot = g_strdup(job->snapshot);
        job->page->last_kind = g_strdup(job->kind);
        job->page->last_file = g_strdup(job->file);

        backup_save_state(job->page);
        backup_update_state_label(job->page);
        backup_refresh_list(job->page);
    }

    backup_job_free(job);
}

static void
on_backup_snapshot_done(gboolean ok,
                        gpointer data)
{
    BackupJob *job = data;
    BackupPage *page = job->page;
    ZgArgs *args;
    gchar *message;
    ZgProgressSpec spec;

    if (!ok)
    {
        backup_job_free(job);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, job->snapshot);

    if (job->base != NULL)
        zg_args_add_option(args, "i", job->base);
    else
        zg_args_add_flag(args, "-R");

    zg_args_set_io(args, "output", job->file);

    message = g_strdup_printf("系统%s备份完成：%s",
                              g_strcmp0(job->kind, "全量") == 0 ? "全量" : "增量",
                              job->file);
    spec = (ZgProgressSpec){
            "zfs.send", "系统备份", job->snapshot, job->file, message, FALSE,
            on_backup_send_done, job};

    zg_dialogs_run_progress(GTK_WINDOW(gtk_widget_get_root(page->root)), &spec, args);
    g_free(message);
}

static void
backup_start(BackupPage *page,
             gboolean incremental)
{
    const gchar *pool = backup_selected_pool(page);
    const gchar *target = gtk_editable_get_text(GTK_EDITABLE(page->target_entry));
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));
    BackupJob *job;
    ZgArgs *args;
    gchar *stamp;
    gchar *snapshot;
    gchar *file;
    gchar *name;

    if (!backup_target_valid(page))
        return;

    if (pool == NULL || !zg_pool_name_valid(pool))
    {
        zg_dialogs_error(window, "没有选择存储池", "请先选择要备份的存储池。");
        return;
    }

    if (incremental && (page->last_snapshot == NULL ||
                        !g_str_has_prefix(page->last_snapshot, pool)))
    {
        zg_dialogs_error(window, "缺少全量基线",
                         "增量备份需要先对同一个池执行一次「全量备份」。");
        return;
    }

    stamp = backup_now_stamp();
    snapshot = g_strdup_printf("%s@" BACKUP_SNAPSHOT_PREFIX "%s", pool, stamp);
    name = g_strdup_printf("%s-%s.zfs", incremental ? "inc" : "full", stamp);
    file = g_build_filename(target, name, NULL);

    job = g_new0(BackupJob, 1);
    job->page = page;
    job->snapshot = snapshot;
    job->file = file;
    job->kind = g_strdup(incremental ? "增量" : "全量");
    job->base = incremental ? g_strdup(page->last_snapshot) : NULL;

    args = zg_args_new();
    zg_args_add_flag(args, "-r");
    zg_args_add_positional(args, snapshot);

    zg_dialogs_run_op(window, "zfs.snapshot", args, NULL,
                      on_backup_snapshot_done, job);

    g_free(name);
    g_free(stamp);
}

static void
on_full_clicked(GtkButton *button,
                gpointer user_data)
{
    (void)button;
    backup_start(user_data, FALSE);
}

static void
on_incremental_clicked(GtkButton *button,
                       gpointer user_data)
{
    (void)button;
    backup_start(user_data, TRUE);
}

/* ------------------------------------------------------------------------- */
/* 备份文件列表                                                               */
/* ------------------------------------------------------------------------- */

static void
backup_clear_list(BackupPage *page)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(page->list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(page->list), child);
}

static void
backup_append_message(BackupPage *page,
                      const gchar *text)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = zg_ui_label_dim(text);

    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 6);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_append(GTK_LIST_BOX(page->list), row);
}

static void
backup_add_file_row(BackupPage *page,
                    const gchar *name,
                    const gchar *path)
{
    GStatBuf info;
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *detail;
    gchar *size_text = g_strdup("—");
    gchar *time_text = g_strdup("—");
    gchar *detail_text;
    GDateTime *mtime;

    if (g_stat(path, &info) == 0)
    {
        g_free(size_text);
        size_text = zg_format_bytes((guint64)info.st_size);

        mtime = g_date_time_new_from_unix_local(info.st_mtime);
        if (mtime != NULL)
        {
            g_free(time_text);
            time_text = g_date_time_format(mtime, "%Y-%m-%d %H:%M");
            g_date_time_unref(mtime);
        }
    }

    detail_text = g_strdup_printf("%s · %s", size_text, time_text);
    detail = zg_ui_label(detail_text);

    gtk_widget_add_css_class(label, "zg-cell-bold");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(detail, "zg-dim");

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), detail);
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "backup-path", g_strdup(path), g_free);
    gtk_list_box_append(GTK_LIST_BOX(page->list), row);

    g_free(size_text);
    g_free(time_text);
    g_free(detail_text);
}

static void
backup_refresh_list(BackupPage *page)
{
    const gchar *target = gtk_editable_get_text(GTK_EDITABLE(page->target_entry));
    GDir *dir;

    backup_clear_list(page);

    if (target == NULL || target[0] != '/' ||
        (dir = g_dir_open(target, 0, NULL)) == NULL)
    {
        backup_append_message(page, "请选择存在的外置盘目录");
        return;
    }

    {
        const gchar *name;
        guint count = 0;

        while ((name = g_dir_read_name(dir)) != NULL)
        {
            gchar *path;

            if (!g_str_has_suffix(name, ".zfs"))
                continue;

            path = g_build_filename(target, name, NULL);
            backup_add_file_row(page, name, path);
            g_free(path);
            count++;
        }

        g_dir_close(dir);

        if (count == 0)
            backup_append_message(page, "目录中没有 .zfs 备份文件");
    }

    gtk_list_box_select_row(GTK_LIST_BOX(page->list),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(page->list), 0));
}

static const gchar *
backup_selected_file(BackupPage *page)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(page->list));

    if (row == NULL)
        return NULL;

    return g_object_get_data(G_OBJECT(row), "backup-path");
}

static void
on_refresh_list_clicked(GtkButton *button,
                        gpointer user_data)
{
    (void)button;
    backup_refresh_list(user_data);
}

/* ------------------------------------------------------------------------- */
/* 删除与恢复                                                                 */
/* ------------------------------------------------------------------------- */

static void
on_delete_done(gboolean ok,
               gpointer data)
{
    BackupPage *page = data;

    if (ok)
    {
        if (page->last_file != NULL &&
            !g_file_test(page->last_file, G_FILE_TEST_EXISTS))
        {
            g_free(page->last_file);
            page->last_file = NULL;
            backup_save_state(page);
            backup_update_state_label(page);
        }

        backup_refresh_list(page);
    }
}

static void
on_delete_clicked(GtkButton *button,
                  gpointer user_data)
{
    BackupPage *page = user_data;
    const gchar *path = backup_selected_file(page);
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));
    ZgArgs *args;

    (void)button;

    if (path == NULL)
    {
        zg_dialogs_error(window, "没有选择备份", "请先在列表中选择一个 .zfs 文件。");
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, path);

    zg_dialogs_run_op(window, "backup.unlink", args, "备份文件已删除",
                      on_delete_done, page);
}

static void
on_restore_target_ready(const gchar *text,
                        gpointer data)
{
    RestoreContext *context = data;
    ZgArgs *args;
    gchar *message;

    if (text == NULL || text[0] == '\0')
    {
        g_free(context->path);
        g_free(context);
        return;
    }

    args = zg_args_new();
    zg_args_add_flag(args, "-F");
    zg_args_add_flag(args, "-d");
    zg_args_add_positional(args, text);
    zg_args_set_io(args, "input", context->path);

    message = g_strdup_printf("备份已恢复到 %s", text);
    zg_dialogs_run_op(GTK_WINDOW(gtk_widget_get_root(context->page->root)),
                      "zfs.receive", args, message, NULL, NULL);

    g_free(message);
    g_free(context->path);
    g_free(context);
}

static void
on_restore_clicked(GtkButton *button,
                   gpointer user_data)
{
    BackupPage *page = user_data;
    const gchar *path = backup_selected_file(page);
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));
    ZgInputSpec spec;

    (void)button;

    if (path == NULL)
    {
        zg_dialogs_error(window, "没有选择备份", "请先在列表中选择一个 .zfs 文件。");
        return;
    }

    spec = (ZgInputSpec){
            "恢复系统备份", "恢复到目标池",
            "会对目标池执行 zfs receive -F -d，目标池中同名数据集会被覆盖。\n"
            "系统无法启动时，请用 FreeBSD 启动盘执行：\n"
            "  zpool import backup\n"
            "  zfs receive -F -d <目标池> < 备份文件",
            "目标池 / 数据集", "例如 restore 或 tank/restore", "restore", "开始恢复",
            TRUE};

    {
        RestoreContext *context = g_new0(RestoreContext, 1);

        context->page = page;
        context->path = g_strdup(path);
        zg_dialogs_input(window, &spec, on_restore_target_ready, context);
    }
}

/* ------------------------------------------------------------------------- */
/* 刷新与构建                                                                 */
/* ------------------------------------------------------------------------- */

static void
on_pools_listed(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
    BackupPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    gchar *keep = g_strdup(backup_selected_pool(page));
    gchar **lines;
    guint i;
    guint target = 0;
    gboolean found = FALSE;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_client_result_free(response);
        g_free(keep);
        return;
    }

    while (g_list_model_get_n_items(G_LIST_MODEL(page->pools)) > 0)
        gtk_string_list_remove(page->pools, 0);

    lines = g_strsplit(response->stdout_text, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        if (lines[i][0] != '\0')
            gtk_string_list_append(page->pools, lines[i]);
    }
    g_strfreev(lines);

    for (i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(page->pools)); i++)
    {
        if (keep != NULL &&
            g_strcmp0(gtk_string_list_get_string(page->pools, i), keep) == 0)
        {
            target = i;
            found = TRUE;
            break;
        }
    }

    if (g_list_model_get_n_items(G_LIST_MODEL(page->pools)) > 0)
    {
        if (!found)
        {
            guint count = g_list_model_get_n_items(G_LIST_MODEL(page->pools));

            target = 0;
            for (i = 0; i < count; i++)
            {
                if (g_strcmp0(gtk_string_list_get_string(page->pools, i), "zroot") == 0)
                {
                    target = i;
                    break;
                }
            }
        }

        gtk_drop_down_set_selected(GTK_DROP_DOWN(page->pool_drop), target);
    }

    g_free(keep);
    zg_client_result_free(response);
}

static void
backup_refresh(gpointer data)
{
    BackupPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_option(args, "o", "name");

    zg_client_call_async("zpool.list", zg_args_to_json(args), on_pools_listed, page);

    backup_refresh_list(page);
}

static void
on_browse_clicked(GtkButton *button,
                  gpointer user_data)
{
    BackupPage *page = user_data;

    (void)button;

    zg_dialogs_pick_folder(GTK_WINDOW(gtk_widget_get_root(page->root)),
                           page->target_entry);
}

GtkWidget *
zg_page_backup_new(ZgApp *app)
{
    BackupPage *page = g_new0(BackupPage, 1);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *grid;
    GtkWidget *buttons;
    GtkWidget *scrolled;
    GtkWidget *hint;
    gint row = 0;

    page->app = app;

    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);

    gtk_box_append(GTK_BOX(root), zg_ui_label_heading("系统备份"));
    gtk_box_append(GTK_BOX(root), zg_ui_label_wrap(
                                          "把整个系统的递归快照以 zfs send 流文件写入外置盘，"
                                          "首次执行全量备份，之后执行增量备份即可。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(root), grid);

    page->pools = gtk_string_list_new(NULL);
    page->pool_drop = gtk_drop_down_new(G_LIST_MODEL(page->pools), NULL);
    gtk_widget_set_hexpand(page->pool_drop, TRUE);
    zg_dialogs_add_field(grid, &row, "源存储池", page->pool_drop,
                         "通常选择 zroot（整个系统）");

    {
        GtkWidget *target_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *browse = gtk_button_new_with_label("浏览…");

        page->target_entry = gtk_entry_new();
        gtk_widget_set_hexpand(page->target_entry, TRUE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(page->target_entry),
                                       "/backup（外置盘挂载点）");
        g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), page);
        gtk_box_append(GTK_BOX(target_row), page->target_entry);
        gtk_box_append(GTK_BOX(target_row), browse);
        zg_dialogs_add_field(grid, &row, "备份目录", target_row,
                             "外置 ZFS 池的挂载点，备份文件写在这里");
    }

    page->state_label = zg_ui_label("还没有备份记录");
    gtk_widget_add_css_class(page->state_label, "zg-dim");
    gtk_label_set_wrap(GTK_LABEL(page->state_label), TRUE);
    gtk_box_append(GTK_BOX(root), page->state_label);

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    {
        GtkWidget *full = zg_ui_button("全量备份", G_CALLBACK(on_full_clicked), page);
        GtkWidget *inc = zg_ui_button("增量备份", G_CALLBACK(on_incremental_clicked), page);

        gtk_widget_add_css_class(full, "suggested-action");
        gtk_box_append(GTK_BOX(buttons), full);
        gtk_box_append(GTK_BOX(buttons), inc);
        gtk_box_append(GTK_BOX(buttons),
                       zg_ui_button("刷新列表", G_CALLBACK(on_refresh_list_clicked), page));
        gtk_box_append(GTK_BOX(buttons),
                       zg_ui_button("删除所选备份", G_CALLBACK(on_delete_clicked), page));
        gtk_box_append(GTK_BOX(buttons),
                       zg_ui_button("恢复所选备份…", G_CALLBACK(on_restore_clicked), page));
    }
    gtk_box_append(GTK_BOX(root), buttons);

    hint = zg_ui_label_wrap(
            "全量备份会创建 recursive 快照（如 zroot@sys-时间戳），并以 -R 发送；"
            "增量备份会自动以上一次成功的快照为起点（-i）。"
            "系统无法启动时，请用 FreeBSD 启动盘导入外置池后再用 zfs receive 恢复。");
    gtk_widget_add_css_class(hint, "zg-dim");
    gtk_box_append(GTK_BOX(root), hint);

    page->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list), GTK_SELECTION_SINGLE);
    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->list);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(root), scrolled);

    g_object_set_data(G_OBJECT(root), "zg-backup-page", page);
    g_object_set_data_full(G_OBJECT(root), "zg-backup-page-free", page,
                           (GDestroyNotify)g_free);

    backup_load_state(page);
    backup_update_state_label(page);
    backup_refresh_list(page);

    zg_app_register_refresh(app, backup_refresh, page);

    return root;
}
