/* zg-page-sendrecv.c - 发送 / 接收页面
 *
 * 以文件为中介完成 zfs send / receive，输出结果直接显示在页面中。
 * 需要管道直连（send | receive）时可使用「命令中心」中的相应操作。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-libzfs.h"
#include "zg-model.h"
#include "zg-opdef.h"
#include "zg-pages.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    ZgApp *app;
    GtkWidget *root;

    GtkWidget *send_snapshot; /* GtkMenuButton */
    GtkWidget *snapshot_label;
    GtkWidget *snapshot_popover;
    GtkWidget *snapshot_search;
    GtkWidget *snapshot_list;
    guint snapshot_selected;
    GtkStringList *snapshots;
    gchar *selected_snapshot;
    GtkWidget *send_output;
    GtkWidget *send_mode;
    GtkWidget *send_base;
    GtkWidget *send_compress;
    GtkWidget *send_dryrun;

    GtkWidget *recv_input;
    GtkWidget *recv_target;
    GtkWidget *recv_force;
    GtkWidget *recv_nomount;
    GtkWidget *recv_dryrun;

    GtkTextBuffer *output_buffer;
    GtkWidget *progress;
    GtkWidget *send_button;
    GtkWidget *recv_button;
    GtkWidget *cancel_button;
    guint64 pending_bytes;
    gboolean has_progress;
    gboolean cancel_requested;
    guint progress_timer;
} SendRecvPage;

/* ------------------------------------------------------------------------- */
/* 执行                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    SendRecvPage *page;
    gchar *command;
} ExecContext;

static void
append_output(SendRecvPage *page,
              const gchar *text)
{
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(page->output_buffer, &end);
    gtk_text_buffer_insert(page->output_buffer, &end, text, -1);
}

static void
on_execute_done(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
    ExecContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GString *text = g_string_new(NULL);

    (void)source;

    g_string_append_printf(text, "\n$ %s\n", context->command);

    if (context->page->cancel_requested)
    {
        g_string_append(text, "[已取消] 用户中止了流任务\n");
        append_output(context->page, text->str);
        zg_app_set_status(context->page->app, "%s 已取消", context->command);
        if (context->page->progress != NULL)
            gtk_widget_set_visible(context->page->progress, FALSE);
        if (context->page->send_button != NULL)
            gtk_widget_set_sensitive(context->page->send_button, TRUE);
        if (context->page->recv_button != NULL)
            gtk_widget_set_sensitive(context->page->recv_button, TRUE);
        g_string_free(text, TRUE);
        zg_client_result_free(response);
        g_free(context->command);
        g_free(context);
        return;
    }

    if (response == NULL)
    {
        g_string_append(text, "[失败] 后台服务无应答\n");
    }
    else if (response->ok)
    {
        g_string_append(text, "[成功] ");
        g_string_append(text, response->message);
        g_string_append_c(text, '\n');

        if (response->stdout_text[0] != '\0')
            g_string_append_printf(text, "%s\n", response->stdout_text);
    }
    else
    {
        g_string_append_printf(text, "[失败] %s\n", response->message);

        if (response->stderr_text[0] != '\0')
            g_string_append_printf(text, "%s\n", response->stderr_text);
    }

    append_output(context->page, text->str);

    zg_app_log(context->page->app, "%s → %s", context->command,
               (response != NULL && response->ok) ? "成功" : "失败");

    zg_app_set_status(context->page->app, "%s %s", context->command,
                      (response != NULL && response->ok) ? "已完成" : "失败");

    if (context->page->cancel_button != NULL)
        gtk_widget_set_visible(context->page->cancel_button, FALSE);

    if (context->page->send_button != NULL)
        gtk_widget_set_sensitive(context->page->send_button, TRUE);
    if (context->page->recv_button != NULL)
        gtk_widget_set_sensitive(context->page->recv_button, TRUE);

    if (context->page->progress != NULL)
    {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(context->page->progress), 0.0);
        gtk_widget_set_visible(context->page->progress, FALSE);
    }

    if (response != NULL && response->ok)
        zg_app_refresh_all(context->page->app);

    g_string_free(text, TRUE);
    zg_client_result_free(response);
    g_free(context->command);
    g_free(context);
}

static gboolean
update_progress_ui(gpointer data)
{
    SendRecvPage *page = data;

    if (!page->has_progress || page->progress == NULL)
        return G_SOURCE_CONTINUE;

    {
        gchar *text = g_strdup_printf("已传输 %.2f MiB",
                                      (double)page->pending_bytes / (1024.0 * 1024.0));

        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), text);
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(page->progress));
        g_free(text);
    }

    page->has_progress = FALSE;
    return G_SOURCE_CONTINUE;
}

static void
on_stream_progress(guint64 bytes,
                   gpointer user_data)
{
    SendRecvPage *page = user_data;

    page->pending_bytes = bytes;
    page->has_progress = TRUE;
}

static void
on_cancel_clicked(GtkButton *button,
                  gpointer user_data)
{
    SendRecvPage *page = user_data;

    page->cancel_requested = TRUE;
    gtk_button_set_label(button, "正在取消…");
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    zg_client_cancel_stream();
}

typedef struct
{
    SendRecvPage *page;
    gchar *op_id;
    gchar *json;
    gchar *file_path;
    gchar *command;
    gboolean for_write;
} StreamStart;

static void
stream_start_free(StreamStart *start)
{
    if (start == NULL)
        return;

    g_free(start->op_id);
    g_free(start->json);
    g_free(start->file_path);
    g_free(start->command);
    g_free(start);
}

static void
execute_stream_go(SendRecvPage *page,
                  const gchar *op_id,
                  gchar *json,
                  const gchar *file_path,
                  gboolean for_write,
                  const gchar *username,
                  const gchar *password,
                  const gchar *command)
{
    ExecContext *context = g_new0(ExecContext, 1);

    context->page = page;
    context->command = g_strdup(command);

    {
        gchar *notice = g_strdup_printf("\n正在执行：%s\n", command);

        append_output(page, notice);
        g_free(notice);
    }

    page->pending_bytes = 0;
    page->has_progress = FALSE;
    page->cancel_requested = FALSE;

    if (page->cancel_button != NULL)
    {
        gtk_button_set_label(GTK_BUTTON(page->cancel_button), "取消");
        gtk_widget_set_sensitive(page->cancel_button, TRUE);
        gtk_widget_set_visible(page->cancel_button, TRUE);
    }

    if (page->progress != NULL)
    {
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), "0 MiB");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), 0.0);
        gtk_widget_set_visible(page->progress, TRUE);
    }

    if (page->send_button != NULL)
        gtk_widget_set_sensitive(page->send_button, FALSE);
    if (page->recv_button != NULL)
        gtk_widget_set_sensitive(page->recv_button, FALSE);

    zg_app_set_status(page->app, "正在执行 %s …", command);
    zg_client_call_stream_async_auth(op_id, json, file_path, for_write,
                                     username, password,
                                     on_stream_progress, page,
                                     on_execute_done, context);
}

static void
on_stream_password(const gchar *username,
                   const gchar *password,
                   gpointer data)
{
    StreamStart *start = data;

    execute_stream_go(start->page, start->op_id, g_strdup(start->json),
                      start->file_path, start->for_write, username, password,
                      start->command);
    stream_start_free(start);
}

static void
execute_stream(SendRecvPage *page,
               const gchar *op_id,
               ZgArgs *args,
               const gchar *file_path,
               gboolean for_write)
{
    gchar *json = zg_args_to_json(args);
    gchar *command = zg_dialogs_describe_args(op_id, json);
    const ZgOpDef *op = zg_op_lookup(op_id);

    if (op != NULL && zg_op_requires_password(op, zg_app_auth_mode(page->app)))
    {
        StreamStart *start = g_new0(StreamStart, 1);
        GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));

        start->page = page;
        start->op_id = g_strdup(op_id);
        start->json = json;
        start->file_path = g_strdup(file_path);
        start->for_write = for_write;
        start->command = command;

        zg_dialogs_password(window, op_id, json, NULL,
                            on_stream_password, start,
                            (GDestroyNotify)stream_start_free, start);
        return;
    }

    execute_stream_go(page, op_id, json, file_path, for_write, NULL, NULL,
                      command);
    g_free(command);
}

/* ------------------------------------------------------------------------- */
/* 发送                                                                       */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 快照列表                                                                   */
/* ------------------------------------------------------------------------- */

#define SEND_NO_SNAPSHOT "（暂无快照）"

static void
clear_focusability(GtkWidget *widget)
{
    GtkWidget *child = gtk_widget_get_first_child(widget);

    gtk_widget_set_focusable(widget, FALSE);

    while (child != NULL)
    {
        clear_focusability(child);
        child = gtk_widget_get_next_sibling(child);
    }
}

static void
snapshot_apply_filter(SendRecvPage *page)
{
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(page->snapshot_search));
    GtkWidget *row = gtk_widget_get_first_child(page->snapshot_list);
    guint index = 0;

    while (row != NULL)
    {
        gboolean visible = TRUE;

        if (text != NULL && text[0] != '\0')
        {
            const gchar *name = gtk_string_list_get_string(page->snapshots, index);

            visible = name != NULL && strstr(name, text) != NULL;
        }

        gtk_widget_set_visible(row, visible);
        row = gtk_widget_get_next_sibling(row);
        index++;
    }
}

static void
snapshot_rebuild_list(SendRecvPage *page)
{
    GtkWidget *child;
    guint i;
    guint count = g_list_model_get_n_items(G_LIST_MODEL(page->snapshots));

    while ((child = gtk_widget_get_first_child(page->snapshot_list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(page->snapshot_list), child);

    for (i = 0; i < count; i++)
    {
        const gchar *name = gtk_string_list_get_string(page->snapshots, i);
        GtkWidget *label = gtk_label_new(name);
        GtkWidget *row = gtk_list_box_row_new();

        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_widget_set_margin_start(label, 8);
        gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 4);
        gtk_widget_set_margin_bottom(label, 4);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        gtk_list_box_append(GTK_LIST_BOX(page->snapshot_list), row);
    }

    snapshot_apply_filter(page);
}

static void
snapshot_select(SendRecvPage *page,
                guint index)
{
    guint count = g_list_model_get_n_items(G_LIST_MODEL(page->snapshots));
    const gchar *name;

    if (index >= count)
        return;

    name = gtk_string_list_get_string(page->snapshots, index);
    page->snapshot_selected = index;

    g_free(page->selected_snapshot);
    page->selected_snapshot =
            (name != NULL && g_strcmp0(name, SEND_NO_SNAPSHOT) != 0)
                    ? g_strdup(name)
                    : NULL;

    gtk_label_set_text(GTK_LABEL(page->snapshot_label),
                       name != NULL ? name : SEND_NO_SNAPSHOT);
    gtk_popover_popdown(GTK_POPOVER(page->snapshot_popover));
}

static void
on_snapshot_row_activated(GtkListBox *list,
                          GtkListBoxRow *row,
                          gpointer user_data)
{
    SendRecvPage *page = user_data;

    (void)list;

    snapshot_select(page, (guint)gtk_list_box_row_get_index(row));
}

static void
on_snapshot_search_changed(GtkSearchEntry *entry,
                           gpointer user_data)
{
    (void)entry;
    snapshot_apply_filter(user_data);
}

static GtkWidget *
build_snapshot_selector(SendRecvPage *page)
{
    GtkWidget *button = gtk_menu_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scrolled = gtk_scrolled_window_new();

    page->snapshot_label = gtk_label_new(SEND_NO_SNAPSHOT);
    gtk_label_set_xalign(GTK_LABEL(page->snapshot_label), 0.0f);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(button), page->snapshot_label);
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), TRUE);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_set_focusable(button, FALSE);

    page->snapshot_search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->snapshot_search),
                                          "搜索…");
    g_signal_connect(page->snapshot_search, "search-changed",
                     G_CALLBACK(on_snapshot_search_changed), page);
    gtk_box_append(GTK_BOX(box), page->snapshot_search);

    page->snapshot_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->snapshot_list),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(page->snapshot_list, "row-activated",
                     G_CALLBACK(on_snapshot_row_activated), page);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled),
                                  page->snapshot_list);
    gtk_widget_set_size_request(scrolled, 420, 320);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    page->snapshot_popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(page->snapshot_popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button),
                                page->snapshot_popover);

    return button;
}

static void
on_snapshot_list(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    SendRecvPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    gchar *keep;
    gchar **lines;
    guint i;
    guint count;
    guint target = 0;
    gboolean found = FALSE;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_client_result_free(response);
        return;
    }

    keep = g_strdup(page->selected_snapshot);

    while (g_list_model_get_n_items(G_LIST_MODEL(page->snapshots)) > 0)
        gtk_string_list_remove(page->snapshots, 0);

    lines = g_strsplit(response->stdout_text, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        if (lines[i][0] != '\0')
            gtk_string_list_append(page->snapshots, lines[i]);
    }

    g_strfreev(lines);

    count = g_list_model_get_n_items(G_LIST_MODEL(page->snapshots));

    if (count == 0)
    {
        gtk_string_list_append(page->snapshots, SEND_NO_SNAPSHOT);
        snapshot_rebuild_list(page);
        snapshot_select(page, 0);
        g_free(keep);
        zg_client_result_free(response);
        return;
    }

    for (i = 0; i < count; i++)
    {
        if (keep != NULL &&
            g_strcmp0(gtk_string_list_get_string(page->snapshots, i), keep) == 0)
        {
            target = i;
            found = TRUE;
            break;
        }
    }

    snapshot_rebuild_list(page);
    snapshot_select(page, found ? target : 0);

    g_free(keep);
    zg_client_result_free(response);
}

static gboolean
populate_snapshots_local(SendRecvPage *page)
{
    GPtrArray *snapshots = zg_libzfs_snapshots();
    gchar *keep = g_strdup(page->selected_snapshot);
    guint i;
    guint target = 0;
    gboolean found = FALSE;

    if (snapshots == NULL || snapshots->len == 0)
    {
        if (snapshots != NULL)
            g_ptr_array_free(snapshots, TRUE);
        g_free(keep);
        return FALSE;
    }

    while (g_list_model_get_n_items(G_LIST_MODEL(page->snapshots)) > 0)
        gtk_string_list_remove(page->snapshots, 0);

    for (i = 0; i < snapshots->len; i++)
    {
        ZgSnapshot *snapshot = g_ptr_array_index(snapshots, i);

        gtk_string_list_append(page->snapshots, snapshot->name);
    }

    for (i = 0; i < snapshots->len; i++)
    {
        ZgSnapshot *snapshot = g_ptr_array_index(snapshots, i);

        if (keep != NULL && g_strcmp0(snapshot->name, keep) == 0)
        {
            target = i;
            found = TRUE;
            break;
        }
    }

    snapshot_rebuild_list(page);
    snapshot_select(page, found ? target : 0);

    g_ptr_array_free(snapshots, TRUE);
    g_free(keep);
    return TRUE;
}

static void
sendrecv_refresh(gpointer data)
{
    SendRecvPage *page = data;
    ZgArgs *args;

    if (populate_snapshots_local(page))
        return;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_option(args, "o", "name");
    zg_args_add_option(args, "t", "snapshot");

    zg_client_call_async("zfs.list", zg_args_to_json(args), on_snapshot_list, page);
}

static void
on_refresh_snapshots(GtkButton *button,
                     gpointer user_data)
{
    (void)button;

    sendrecv_refresh(user_data);
}

static gchar *
suggest_output_name(const gchar *snapshot)
{
    gchar *name;
    gchar *result;

    if (snapshot == NULL || snapshot[0] == '\0')
        return g_strdup("snapshot.zfs");

    name = g_strdup(snapshot);
    g_strdelimit(name, "/@", '-');
    result = g_strdup_printf("%s.zfs", name);
    g_free(name);

    return result;
}

static void
on_pick_send_output(GtkButton *button,
                    gpointer user_data)
{
    SendRecvPage *page = user_data;
    gchar *name = suggest_output_name(page->selected_snapshot);
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));

    (void)button;

    zg_dialogs_pick_save(window, page->send_output, name);
    g_free(name);
}

static void
on_pick_recv_input(GtkButton *button,
                   gpointer user_data)
{
    SendRecvPage *page = user_data;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));

    (void)button;

    zg_dialogs_pick_open(window, page->recv_input);
}

static void
on_send_clicked(GtkButton *button,
                gpointer user_data)
{
    SendRecvPage *page = user_data;
    const gchar *snapshot = page->selected_snapshot;
    const gchar *output = gtk_editable_get_text(GTK_EDITABLE(page->send_output));
    const gchar *base = gtk_editable_get_text(GTK_EDITABLE(page->send_base));
    const guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->send_mode));
    ZgArgs *args;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));

    (void)button;

    if (snapshot == NULL)
    {
        guint index = page->snapshot_selected;

        if (index < g_list_model_get_n_items(G_LIST_MODEL(page->snapshots)))
            snapshot = gtk_string_list_get_string(page->snapshots, index);
    }

    if (!zg_snapshot_name_valid(snapshot))
    {
        zg_dialogs_error(window, "请选择快照",
                         "请先从「源快照」下拉框中选择一个快照；"
                         "若列表为空，点右侧刷新按钮重新读取。");
        return;
    }

    if (output == NULL || output[0] != '/')
    {
        zg_dialogs_error(window, "输出路径非法", "请填写绝对路径。");
        return;
    }

    if ((mode == 1 || mode == 3) && !zg_snapshot_name_valid(base))
    {
        zg_dialogs_error(window, "增量起点非法",
                         "增量发送需要填写起始快照，例如 tank/data@base。");
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, snapshot);

    if (mode == 2)
        zg_args_add_flag(args, "-R");

    if (mode == 1)
        zg_args_add_option(args, "i", base);

    if (mode == 3)
        zg_args_add_option(args, "I", base);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(page->send_compress)))
        zg_args_add_flag(args, "-c");

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(page->send_dryrun)))
        zg_args_add_flag(args, "-n");

    zg_args_set_io(args, "output", output);

    execute_stream(page, "zfs.send", args, output, TRUE);
}

/* ------------------------------------------------------------------------- */
/* 接收                                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    SendRecvPage *page;
    gchar *target;
    gchar *input;
    gboolean force;
    gboolean nomount;
    gboolean dryrun;
} ReceiveRequest;

static void
receive_request_start(gpointer data)
{
    ReceiveRequest *request = data;
    ZgArgs *args = zg_args_new();

    zg_args_add_positional(args, request->target);

    if (request->force)
        zg_args_add_flag(args, "-F");

    if (request->nomount)
        zg_args_add_flag(args, "-u");

    if (request->dryrun)
        zg_args_add_flag(args, "-n");

    zg_args_set_io(args, "input", request->input);

    execute_stream(request->page, "zfs.receive", args, request->input, FALSE);

    g_free(request->target);
    g_free(request->input);
    g_free(request);
}

static void
on_receive_clicked(GtkButton *button,
                   gpointer user_data)
{
    SendRecvPage *page = user_data;
    const gchar *input = gtk_editable_get_text(GTK_EDITABLE(page->recv_input));
    const gchar *target = gtk_editable_get_text(GTK_EDITABLE(page->recv_target));
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(page->root));
    ReceiveRequest *request;

    (void)button;

    if (input == NULL || input[0] != '/')
    {
        zg_dialogs_error(window, "输入路径非法", "请填写快照流文件的绝对路径。");
        return;
    }

    if (!zg_dataset_name_valid(target))
    {
        zg_dialogs_error(window, "目标数据集非法",
                         "请填写目标数据集，例如 backup/tank-data。");
        return;
    }

    request = g_new0(ReceiveRequest, 1);
    request->page = page;
    request->target = g_strdup(target);
    request->input = g_strdup(input);
    request->force = gtk_check_button_get_active(GTK_CHECK_BUTTON(page->recv_force));
    request->nomount = gtk_check_button_get_active(GTK_CHECK_BUTTON(page->recv_nomount));
    request->dryrun = gtk_check_button_get_active(GTK_CHECK_BUTTON(page->recv_dryrun));

    if (request->force)
    {
        gchar *detail = g_strdup_printf(
                "接收时使用 -F 会丢弃目标数据集上早于流中快照的本地数据。\n\n"
                "目标：%s\n输入：%s",
                target, input);

        zg_dialogs_confirm(window, "确认强制接收", detail, "继续接收", TRUE,
                           receive_request_start, request);
        g_free(detail);
        return;
    }

    receive_request_start(request);
}

static void
on_send_mode_changed(GObject *object,
                     GParamSpec *pspec,
                     gpointer user_data)
{
    SendRecvPage *page = user_data;
    const guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(page->send_mode));

    (void)object;
    (void)pspec;

    gtk_widget_set_sensitive(page->send_base, mode == 1 || mode == 3);
}

static void
on_clear_output(GtkButton *button,
                gpointer user_data)
{
    SendRecvPage *page = user_data;

    (void)button;

    gtk_text_buffer_set_text(page->output_buffer, "", -1);
}

/* ------------------------------------------------------------------------- */
/* 构建界面                                                                   */
/* ------------------------------------------------------------------------- */

static GtkWidget *
build_send_group(SendRecvPage *page)
{
    static const gchar *const modes[] = {
            "全量发送", "增量 (-i)", "复制 (-R，含属性与后代)", "增量含中间快照 (-I)", NULL};
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *button;
    gint row = 0;

    gtk_box_append(GTK_BOX(frame), zg_ui_label_heading("发送快照到文件"));
    gtk_box_append(GTK_BOX(frame), zg_ui_label_dim(
                                           "把快照打包为可传输的流文件；接收端可用本页「接收」还原。"));

    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 6);

    page->snapshots = gtk_string_list_new(NULL);
    page->send_snapshot = build_snapshot_selector(page);
    {
        GtkWidget *snapshot_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *refresh = gtk_button_new_from_icon_name("zg-refresh-symbolic");

        gtk_widget_set_tooltip_text(refresh, "重新读取系统快照列表");
        g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_snapshots), page);
        gtk_box_append(GTK_BOX(snapshot_row), page->send_snapshot);
        gtk_box_append(GTK_BOX(snapshot_row), refresh);
        zg_dialogs_add_field(grid, &row, "源快照", snapshot_row,
                             "从系统已有快照中选择，可按名称搜索");
    }
    {
        GtkWidget *output_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *pick = gtk_button_new_from_icon_name("zg-folder-open-symbolic");

        page->send_output = gtk_entry_new();
        gtk_widget_set_hexpand(page->send_output, TRUE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(page->send_output),
                                       "/backup/tank-data.zfs");
        gtk_widget_set_tooltip_text(pick, "选择保存位置");
        g_signal_connect(pick, "clicked", G_CALLBACK(on_pick_send_output), page);
        gtk_box_append(GTK_BOX(output_row), page->send_output);
        gtk_box_append(GTK_BOX(output_row), pick);
        zg_dialogs_add_field(grid, &row, "输出文件", output_row,
                             "可直接输入，或点右侧图标选择保存位置");
    }

    page->send_mode = gtk_drop_down_new_from_strings(modes);
    clear_focusability(page->send_mode);
    zg_dialogs_add_field(grid, &row, "模式", page->send_mode, NULL);

    page->send_base = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(page->send_base), "tank/data@base");
    gtk_widget_set_sensitive(page->send_base, FALSE);
    zg_dialogs_add_field(grid, &row, "起始快照", page->send_base, "仅增量模式需要");

    gtk_box_append(GTK_BOX(frame), grid);

    page->send_compress = gtk_check_button_new_with_label("压缩数据流 (-c)");
    gtk_box_append(GTK_BOX(frame), page->send_compress);

    page->send_dryrun = gtk_check_button_new_with_label("仅演练 (-n)");
    gtk_box_append(GTK_BOX(frame), page->send_dryrun);

    button = zg_ui_button("开始发送", G_CALLBACK(on_send_clicked), page);
    page->send_button = button;
    gtk_widget_add_css_class(button, "suggested-action");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(frame), button);

    g_signal_connect(page->send_mode, "notify::selected",
                     G_CALLBACK(on_send_mode_changed), page);

    return frame;
}

static GtkWidget *
build_receive_group(SendRecvPage *page)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *button;
    gint row = 0;

    gtk_box_append(GTK_BOX(frame), zg_ui_label_heading("从文件接收快照流"));
    gtk_box_append(GTK_BOX(frame), zg_ui_label_dim(
                                           "把发送得到的流文件还原为数据集；必要时可强制覆盖或仅演练。"));

    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 6);

    {
        GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *pick = gtk_button_new_from_icon_name("zg-open-symbolic");

        page->recv_input = gtk_entry_new();
        gtk_widget_set_hexpand(page->recv_input, TRUE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(page->recv_input),
                                       "/backup/tank-data.zfs");
        gtk_widget_set_tooltip_text(pick, "选择快照流文件");
        g_signal_connect(pick, "clicked", G_CALLBACK(on_pick_recv_input), page);
        gtk_box_append(GTK_BOX(input_row), page->recv_input);
        gtk_box_append(GTK_BOX(input_row), pick);
        zg_dialogs_add_field(grid, &row, "输入文件", input_row,
                             "可直接输入，或点右侧图标选择文件");
    }

    page->recv_target = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(page->recv_target), "backup/tank-data");
    zg_dialogs_add_field(grid, &row, "目标数据集", page->recv_target, NULL);

    gtk_box_append(GTK_BOX(frame), grid);

    page->recv_force = gtk_check_button_new_with_label("强制接收 (-F，可能丢弃本地数据)");
    gtk_box_append(GTK_BOX(frame), page->recv_force);

    page->recv_nomount = gtk_check_button_new_with_label("接收后不挂载 (-u)");
    gtk_box_append(GTK_BOX(frame), page->recv_nomount);

    page->recv_dryrun = gtk_check_button_new_with_label("仅演练 (-n)");
    gtk_box_append(GTK_BOX(frame), page->recv_dryrun);

    button = zg_ui_button("开始接收", G_CALLBACK(on_receive_clicked), page);
    page->recv_button = button;
    gtk_widget_add_css_class(button, "suggested-action");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(frame), button);

    return frame;
}

static void
sendrecv_page_destroy(GtkWidget *widget,
                      gpointer user_data)
{
    SendRecvPage *page = user_data;

    (void)widget;

    if (page->progress_timer != 0)
    {
        g_source_remove(page->progress_timer);
        page->progress_timer = 0;
    }
}

GtkWidget *
zg_page_sendrecv_new(ZgApp *app)
{
    SendRecvPage *page = g_new0(SendRecvPage, 1);
    GtkWidget *scrolled;
    GtkWidget *content;
    GtkWidget *columns;
    GtkWidget *output_scrolled;
    GtkWidget *output_row;

    page->app = app;

    scrolled = gtk_scrolled_window_new();
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_box_set_homogeneous(GTK_BOX(columns), TRUE);
    gtk_box_append(GTK_BOX(columns), build_send_group(page));
    gtk_box_append(GTK_BOX(columns), build_receive_group(page));
    gtk_box_append(GTK_BOX(content), columns);

    gtk_box_append(GTK_BOX(content), zg_ui_label_heading("执行输出"));

    output_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(output_row, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(output_row),
                   zg_ui_button("清空输出", G_CALLBACK(on_clear_output), page));
    gtk_box_append(GTK_BOX(content), output_row);

    {
        GtkWidget *progress_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        page->progress = gtk_progress_bar_new();
        gtk_widget_set_hexpand(page->progress, TRUE);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page->progress), TRUE);
        gtk_widget_set_visible(page->progress, FALSE);
        gtk_box_append(GTK_BOX(progress_row), page->progress);

        page->cancel_button = zg_ui_button("取消", G_CALLBACK(on_cancel_clicked), page);
        gtk_widget_set_visible(page->cancel_button, FALSE);
        gtk_box_append(GTK_BOX(progress_row), page->cancel_button);

        gtk_box_append(GTK_BOX(content), progress_row);
    }

    page->progress_timer = g_timeout_add(100, update_progress_ui, page);

    output_scrolled = zg_ui_text_view_new(&page->output_buffer, TRUE);
    gtk_widget_set_size_request(output_scrolled, -1, 240);
    gtk_box_append(GTK_BOX(content), output_scrolled);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);

    page->root = scrolled;

    g_signal_connect(page->root, "destroy",
                     G_CALLBACK(sendrecv_page_destroy), page);

    g_object_set_data_full(G_OBJECT(page->root), "zg-page-sendrecv", page, g_free);

    zg_app_register_refresh(app, sendrecv_refresh, page);

    return page->root;
}
