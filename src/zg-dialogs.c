/* zg-dialogs.c - 对话框与操作执行实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-dialogs.h"

#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

#include "zg-app.h"
#include "zg-argv.h"
#include "zg-json.h"
#include "zg-model.h"
#include "zg-protocol.h"
#include "zg-ui.h"
#include "zg-util.h"

/* ------------------------------------------------------------------------- */
/* 通用窗口构件                                                               */
/* ------------------------------------------------------------------------- */

/* Esc 关闭当前弹窗 */
static gboolean
on_dialog_key_pressed(GtkEventControllerKey *controller,
                      guint keyval,
                      guint keycode,
                      GdkModifierType state,
                      gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;

    if (keyval == GDK_KEY_Escape)
    {
        gtk_window_close(GTK_WINDOW(user_data));
        return TRUE;
    }

    return FALSE;
}

GtkWindow *
zg_dialogs_window_new(GtkWindow *parent,
                      const gchar *title,
                      gint width,
                      gint height)
{
    GtkWidget *window = gtk_window_new();
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *close;
    GtkEventController *keys;

    gtk_window_set_title(GTK_WINDOW(window), title);

    /* 部分主题（Breeze）会隐藏 GTK 原生窗口按钮，这里自绘关闭按钮 */
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "zg-window-control");
    gtk_widget_set_tooltip_text(close, "关闭 (Esc)");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), window);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), close);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_dialog_key_pressed), window);
    gtk_widget_add_controller(window, keys);

    if (width > 0 && height > 0)
        gtk_window_set_default_size(GTK_WINDOW(window), width, height);

    if (parent != NULL)
    {
        gtk_window_set_transient_for(GTK_WINDOW(window), parent);
        gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    }

    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);

    return GTK_WINDOW(window);
}

GtkWidget *
zg_dialogs_body(GtkWindow *window,
                GtkWidget **out_box)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);

    gtk_window_set_child(window, box);

    if (out_box != NULL)
        *out_box = box;

    return box;
}

GtkWidget *
zg_dialogs_button_row(GtkWindow *window,
                      GtkWidget **out_cancel,
                      GtkWidget **out_accept,
                      const gchar *accept_label,
                      gboolean destructive)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *cancel;
    GtkWidget *accept;
    GtkWidget *parent = gtk_window_get_child(window);

    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 6);

    cancel = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel);

    accept = gtk_button_new_with_label(accept_label != NULL ? accept_label : "确定");

    if (destructive)
        gtk_widget_add_css_class(accept, "destructive-action");
    else
        gtk_widget_add_css_class(accept, "suggested-action");

    gtk_box_append(GTK_BOX(row), accept);

    if (parent != NULL)
        gtk_box_append(GTK_BOX(parent), row);

    if (out_cancel != NULL)
        *out_cancel = cancel;

    if (out_accept != NULL)
        *out_accept = accept;

    return row;
}

void zg_dialogs_add_field(GtkWidget *grid,
                          gint *row,
                          const gchar *label,
                          GtkWidget *widget,
                          const gchar *hint)
{
    GtkWidget *name = zg_ui_label(label);
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    gtk_widget_set_hexpand(widget, TRUE);
    gtk_box_append(GTK_BOX(cell), widget);

    if (hint != NULL && hint[0] != '\0')
        gtk_box_append(GTK_BOX(cell), zg_ui_label_dim(hint));

    gtk_grid_attach(GTK_GRID(grid), name, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cell, 1, *row, 1, 1);

    (*row)++;
}

/* ------------------------------------------------------------------------- */
/* 提示框                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkTextBuffer *buffer;
} OutputDialog;

static void
on_output_close(GtkButton *button,
                gpointer user_data)
{
    OutputDialog *dialog = user_data;

    (void)button;

    gtk_window_destroy(GTK_WINDOW(dialog->window));
}

void zg_dialogs_show_output(GtkWindow *parent,
                            const gchar *title,
                            const gchar *text)
{
    OutputDialog *dialog = g_new0(OutputDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *scrolled;
    GtkWidget *row;
    GtkWidget *close_button;

    window = zg_dialogs_window_new(parent, title != NULL ? title : "命令输出", 820, 560);
    body = zg_dialogs_body(window, NULL);

    scrolled = zg_ui_text_view_new(&dialog->buffer, FALSE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(body), scrolled);
    zg_ui_text_set(dialog->buffer, text);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    close_button = gtk_button_new_with_label("关闭");
    gtk_widget_add_css_class(close_button, "suggested-action");
    gtk_box_append(GTK_BOX(row), close_button);
    gtk_box_append(GTK_BOX(body), row);

    dialog->window = GTK_WIDGET(window);

    g_signal_connect(close_button, "clicked", G_CALLBACK(on_output_close), dialog);
    g_object_set_data_full(G_OBJECT(window), "zg-output-dialog", dialog, g_free);

    gtk_window_present(window);
}

static void
show_message(GtkWindow *parent,
             const gchar *title,
             const gchar *heading,
             const gchar *detail,
             gboolean error)
{
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *head;
    GtkWidget *row;
    GtkWidget *close_button;

    window = zg_dialogs_window_new(parent, title, 660, 300);
    body = zg_dialogs_body(window, NULL);

    head = zg_ui_label_heading(heading);
    if (error)
        gtk_widget_add_css_class(head, "zg-danger");

    gtk_box_append(GTK_BOX(body), head);

    if (detail != NULL && detail[0] != '\0')
    {
        GtkWidget *scrolled = gtk_scrolled_window_new();
        GtkWidget *label = zg_ui_label_wrap(detail);

        gtk_widget_set_vexpand(scrolled, TRUE);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), label);
        gtk_box_append(GTK_BOX(body), scrolled);
    }

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    close_button = gtk_button_new_with_label("关闭");
    gtk_widget_add_css_class(close_button, "suggested-action");
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), close_button);
    gtk_box_append(GTK_BOX(body), row);

    gtk_window_present(window);
}

void zg_dialogs_error(GtkWindow *parent,
                      const gchar *title,
                      const gchar *detail)
{
    show_message(parent, "错误", title != NULL ? title : "操作失败", detail, TRUE);
}

void zg_dialogs_info(GtkWindow *parent,
                     const gchar *title,
                     const gchar *detail)
{
    show_message(parent, "提示", title != NULL ? title : "操作完成", detail, FALSE);
}

typedef struct
{
    ZgConfirmFunc callback;
    gpointer data;
} ConfirmData;

static void
on_confirm_accept(GtkButton *button,
                  gpointer user_data)
{
    GtkWidget *window = user_data;
    ConfirmData *confirm = g_object_get_data(G_OBJECT(window), "zg-confirm");
    ZgConfirmFunc callback = NULL;
    gpointer data = NULL;

    (void)button;

    if (confirm != NULL)
    {
        callback = confirm->callback;
        data = confirm->data;
    }

    gtk_window_destroy(GTK_WINDOW(window));

    if (callback != NULL)
        callback(data);
}

void zg_dialogs_confirm(GtkWindow *parent,
                        const gchar *title,
                        const gchar *detail,
                        const gchar *accept_label,
                        gboolean destructive,
                        ZgConfirmFunc on_accept,
                        gpointer data)
{
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *head;
    GtkWidget *scrolled;
    GtkWidget *label;
    GtkWidget *cancel_button;
    GtkWidget *accept_button;

    window = zg_dialogs_window_new(parent, title != NULL ? title : "确认操作", 720, 380);
    body = zg_dialogs_body(window, NULL);

    head = zg_ui_label_heading(title != NULL ? title : "确认操作");

    if (destructive)
        gtk_widget_add_css_class(head, "zg-danger");

    gtk_box_append(GTK_BOX(body), head);

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    label = zg_ui_label_wrap(detail);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), label);
    gtk_box_append(GTK_BOX(body), scrolled);

    zg_dialogs_button_row(window, &cancel_button, &accept_button,
                          accept_label != NULL ? accept_label : "继续",
                          destructive);

    {
        ConfirmData *confirm = g_new0(ConfirmData, 1);

        confirm->callback = on_accept;
        confirm->data = data;
        g_object_set_data_full(G_OBJECT(window), "zg-confirm", confirm, g_free);
    }

    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_confirm_accept), window);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 命令描述与标签                                                             */
/* ------------------------------------------------------------------------- */

gchar *
zg_dialogs_describe_args(const gchar *op_id,
                         const gchar *args_json)
{
    const ZgOpDef *op;
    ZgJson *node;
    ZgOpRequest *request;
    gchar *text = NULL;
    GError *error = NULL;

    op = zg_op_lookup(op_id);

    if (op == NULL)
        return g_strdup(op_id != NULL ? op_id : "(未知操作)");

    node = zg_json_parse(args_json != NULL ? args_json : "{}", -1, &error);

    if (node == NULL)
    {
        g_clear_error(&error);
        return g_strdup(op->id);
    }

    request = zg_op_request_new();

    if (zg_op_request_parse(node, request, &error))
        text = zg_op_describe(op, request);

    g_clear_error(&error);
    zg_op_request_free(request);
    zg_json_free(node);

    return (text != NULL) ? text : g_strdup(op->id);
}

static const gchar *
flag_label(const gchar *flag)
{
    static const struct
    {
        const gchar *flag;
        const gchar *label;
    } labels[] = {
            {"-f", "强制 (-f)"},
            {"-n", "演练，不做实际修改 (-n)"},
            {"-v", "详细输出 (-v)"},
            {"-r", "递归处理子级 (-r)"},
            {"-R", "递归并包含克隆/属性 (-R)"},
            {"-p", "精确数值 / 创建父级 / 保留路径 (-p)"},
            {"-H", "脚本模式，制表符分隔 (-H)"},
            {"-j", "JSON 输出 (-j)"},
            {"-a", "作用于全部 (-a)"},
            {"-u", "不挂载 (-u)"},
            {"-d", "显示详情 / 指定目录 / 限制深度 (-d)"},
            {"-e", "扩展或排除 (-e)"},
            {"-w", "等待操作完成 (-w)"},
            {"-s", "顺序执行 / 稀疏 / 停止 (-s)"},
            {"-l", "长格式 (-l)"},
            {"-i", "增量 (-i)"},
            {"-I", "增量并包含中间快照 (-I)"},
            {"-F", "强制回滚事务 (-F)"},
            {"-L", "长格式 (-L)"},
            {"-P", "显示设备路径 (-P)"},
            {"-g", "显示 GUID (-g)"},
            {"-x", "仅显示异常 / 排除属性 (-x)"},
            {"-t", "临时 / 按类型过滤 (-t)"},
            {"-D", "详细诊断 (-D)"},
            {"-C", "取消/清理 (-C)"},
            {"-S", "停止 (-S)"},
            {"-E", "结束时间 (-E)"},
            {"-T", "时间格式 (-T)"},
            {"-c", "取消 / 压缩 / 脚本 (-c)"},
            {"-h", "表头 / 人类可读 (-h)"},
            {"-M", "接收时强制覆盖 (-M)"},
            {"-N", "不挂载 (-N)"},
            {"-V", "指定版本 (-V)"},
            {"-b", "块大小 / 内嵌数据 (-b)"},
            {"-y", "跳过表头 (-y)"},
            {"-q", "安静模式 (-q)"},
            {"-0", "以 NUL 分隔 (-0)"},
            {"-k", "保留现有数据 (-k)"},
            {"-U", "使用 jail 配置 (-U)"},
            {"-B", "后台运行 (-B)"},
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(labels); i++)
    {
        if (g_strcmp0(labels[i].flag, flag) == 0)
            return labels[i].label;
    }

    return flag;
}

static const gchar *
option_label(const gchar *key)
{
    static const struct
    {
        const gchar *key;
        const gchar *label;
    } labels[] = {
            {"o", "属性 (-o key=value)"},
            {"O", "文件系统属性 (-O key=value)"},
            {"m", "挂载点 (-m)"},
            {"R", "备用根目录 altroot (-R)"},
            {"d", "搜索目录 / 深度 (-d)"},
            {"c", "缓存文件 / 脚本 / 模式 (-c)"},
            {"V", "版本 / 卷大小 (-V)"},
            {"b", "块大小 (-b)"},
            {"t", "类型 (-t)"},
            {"x", "排除属性 (-x)"},
            {"s", "排序字段 / 来源 (-s)"},
            {"S", "排序字段 / 来源 (-S)"},
            {"T", "时间单位 (-T)"},
            {"i", "增量起点快照 (-i)"},
            {"I", "增量起点（含中间快照）(-I)"},
            {"X", "排除数据集 (-X)"},
            {"L", "密钥位置 (-L)"},
            {"r", "限速 / 来源 (-r)"},
            {"l", "长度 (-l)"},
            {"g", "GUID (-g)"},
            {"p", "项目 ID (-p)"},
            {"e", "来源启动环境 (-e)"},
            {"E", "结束日期 (-E)"},
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(labels); i++)
    {
        if (g_strcmp0(labels[i].key, key) == 0)
            return labels[i].label;
    }

    return key;
}

/* ------------------------------------------------------------------------- */
/* 管理员密码二次验证                                                         */
/* ------------------------------------------------------------------------- */

typedef void (*ZgPasswordFunc)(const gchar *username,
                               const gchar *password,
                               gpointer data);

typedef struct
{
    GtkWidget *window;
    GtkWidget *user_entry;
    GtkWidget *pass_entry;
    GtkWidget *error_label;
    gchar *op_id;
    gchar *args_json;
    ZgPasswordFunc callback;
    gpointer data;
    gboolean submitted;
    GDestroyNotify cancel_cleanup;
    gpointer cancel_data;
} PasswordDialog;

static void
password_dialog_free(gpointer data)
{
    PasswordDialog *dialog = data;

    if (dialog == NULL)
        return;

    if (!dialog->submitted && dialog->cancel_cleanup != NULL)
        dialog->cancel_cleanup(dialog->cancel_data);

    g_free(dialog->op_id);
    g_free(dialog->args_json);
    g_free(dialog);
}

static void
password_dialog_set_error(PasswordDialog *dialog,
                          const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(dialog->error_label), text);
    gtk_widget_set_visible(dialog->error_label, text != NULL && text[0] != '\0');
}

static void
on_password_submit(GtkButton *button,
                   gpointer user_data)
{
    PasswordDialog *dialog = user_data;
    const gchar *username = gtk_editable_get_text(GTK_EDITABLE(dialog->user_entry));
    const gchar *password = gtk_editable_get_text(GTK_EDITABLE(dialog->pass_entry));
    ZgPasswordFunc callback = dialog->callback;
    gpointer callback_data = dialog->data;

    (void)button;

    if (username == NULL || username[0] == '\0')
    {
        password_dialog_set_error(dialog, "请输入管理员账号");
        return;
    }

    if (password == NULL || password[0] == '\0')
    {
        password_dialog_set_error(dialog, "请输入密码");
        return;
    }

    {
        gchar *user_copy = g_strdup(username);
        gchar *pass_copy = g_strdup(password);

        dialog->submitted = TRUE;
        gtk_window_destroy(GTK_WINDOW(dialog->window));

        callback(user_copy, pass_copy, callback_data);

        explicit_bzero(pass_copy, strlen(pass_copy));
        g_free(pass_copy);
        g_free(user_copy);
    }
}

static void
on_password_visibility(GtkToggleButton *toggle,
                       gpointer user_data)
{
    PasswordDialog *dialog = user_data;

    gtk_entry_set_visibility(GTK_ENTRY(dialog->pass_entry),
                             gtk_toggle_button_get_active(toggle));
}

static void
zg_dialogs_password(GtkWindow *parent,
                    const gchar *op_id,
                    const gchar *args_json,
                    const gchar *error_text,
                    ZgPasswordFunc callback,
                    gpointer data,
                    GDestroyNotify cancel_cleanup,
                    gpointer cancel_data)
{
    const ZgOpDef *op = zg_op_lookup(op_id);
    PasswordDialog *dialog = g_new0(PasswordDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *header;
    GtkWidget *icon;
    GtkWidget *texts;
    GtkWidget *card;
    GtkWidget *command_label;
    GtkWidget *grid;
    GtkWidget *pass_row;
    GtkWidget *toggle;
    GtkWidget *row;
    GtkWidget *cancel_button;
    GtkWidget *accept_button;
    gchar *command;
    gint grid_row = 0;

    window = zg_dialogs_window_new(parent, "需要管理员授权", 640, 500);
    dialog->window = GTK_WIDGET(window);
    dialog->op_id = g_strdup(op_id);
    dialog->args_json = g_strdup(args_json);
    dialog->callback = callback;
    dialog->data = data;
    dialog->cancel_cleanup = cancel_cleanup;
    dialog->cancel_data = cancel_data;

    body = zg_dialogs_body(window, NULL);

    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    icon = gtk_image_new_from_icon_name("channel-secure-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 42);
    gtk_widget_add_css_class(icon, "zg-lock-icon");

    texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_append(GTK_BOX(texts),
                   zg_ui_label_heading(op != NULL ? op->title : op_id));
    gtk_box_append(GTK_BOX(texts), zg_ui_label_dim(
                                           "该操作会修改 ZFS 存储状态，属于危险操作，需要管理员密码确认。"));

    gtk_box_append(GTK_BOX(header), icon);
    gtk_box_append(GTK_BOX(header), texts);
    gtk_box_append(GTK_BOX(body), header);

    gtk_box_append(GTK_BOX(body), zg_ui_label_dim("即将执行"));

    card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "zg-card");
    gtk_widget_set_hexpand(card, TRUE);

    command = zg_dialogs_describe_args(op_id, args_json);
    command_label = zg_ui_label_mono(command);
    gtk_label_set_xalign(GTK_LABEL(command_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(command_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(command_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(command_label), 3);
    gtk_label_set_ellipsize(GTK_LABEL(command_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(command_label, TRUE);

    gtk_box_append(GTK_BOX(card), command_label);
    gtk_box_append(GTK_BOX(body), card);
    g_free(command);

    dialog->error_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(dialog->error_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dialog->error_label), 0.0);
    gtk_widget_add_css_class(dialog->error_label, "zg-danger");
    gtk_widget_set_visible(dialog->error_label,
                           error_text != NULL && error_text[0] != '\0');

    if (error_text != NULL)
        gtk_label_set_text(GTK_LABEL(dialog->error_label), error_text);

    gtk_box_append(GTK_BOX(body), dialog->error_label);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 4);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->user_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dialog->user_entry), g_get_user_name());
    zg_dialogs_add_field(grid, &grid_row, "管理员账号", dialog->user_entry,
                         "需为 root，或属于 wheel 等 allowed_groups");

    dialog->pass_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(dialog->pass_entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(dialog->pass_entry),
                                GTK_INPUT_PURPOSE_PASSWORD);
    gtk_editable_set_text(GTK_EDITABLE(dialog->pass_entry), "");

    toggle = gtk_toggle_button_new_with_label("显示");
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    g_signal_connect(toggle, "toggled", G_CALLBACK(on_password_visibility), dialog);

    pass_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(dialog->pass_entry, TRUE);
    gtk_box_append(GTK_BOX(pass_row), dialog->pass_entry);
    gtk_box_append(GTK_BOX(pass_row), toggle);

    zg_dialogs_add_field(grid, &grid_row, "密码", pass_row,
                         "仅用于本次验证，不会被保存或记录");

    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "验证由系统 PAM 完成；连续多次失败会触发临时锁定。"));

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 6);

    cancel_button = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel_button);

    accept_button = gtk_button_new_with_label("验证并执行");
    gtk_widget_add_css_class(accept_button, "suggested-action");
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_password_submit), dialog);
    gtk_box_append(GTK_BOX(row), accept_button);

    gtk_box_append(GTK_BOX(body), row);

    g_signal_connect(dialog->pass_entry, "activate",
                     G_CALLBACK(on_password_submit), dialog);
    g_signal_connect(dialog->user_entry, "activate",
                     G_CALLBACK(on_password_submit), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-password", dialog, password_dialog_free);

    gtk_window_present(window);
    gtk_widget_grab_focus(dialog->pass_entry);
}

/* ------------------------------------------------------------------------- */
/* 操作执行                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    gchar *op_id;
    gchar *args_json;
    gchar *command;
    gchar *success_message;
    ZgDialogDoneFunc done;
    gpointer user_data;
    guint attempts;
} RunOp;

static void
run_op_free(RunOp *run)
{
    if (run == NULL)
        return;

    g_free(run->op_id);
    g_free(run->args_json);
    g_free(run->command);
    g_free(run->success_message);
    g_free(run);
}

static void on_run_op_done(GObject *source,
                           GAsyncResult *async_result,
                           gpointer user_data);

static gboolean
result_is_auth_problem(const ZgClientResult *result)
{
    return result != NULL &&
           (result->code == ZG_ERR_AUTH_REQUIRED ||
            result->code == ZG_ERR_AUTH_FAILED);
}

static void
run_op_execute(RunOp *run,
               const gchar *username,
               const gchar *password)
{
    ZgApp *app = zg_app_get_default();

    run->attempts++;

    zg_app_set_status(app, "正在执行：%s", run->command);

    if (username != NULL)
        zg_app_log(app, "已通过管理员认证，执行 %s", run->command);
    else
        zg_app_log(app, "执行 %s", run->command);

    zg_client_call_async_auth(run->op_id, g_strdup(run->args_json),
                              username, password,
                              on_run_op_done, run);
}

static void
on_password_ready(const gchar *username,
                  const gchar *password,
                  gpointer data)
{
    run_op_execute(data, username, password);
}

static void
on_run_op_confirmed(gpointer data)
{
    run_op_execute(data, NULL, NULL);
}

static void
on_run_op_done(GObject *source,
               GAsyncResult *async_result,
               gpointer user_data)
{
    RunOp *run = user_data;
    ZgApp *app = zg_app_get_default();
    GtkWindow *parent = zg_app_get_window(app);
    ZgClientResult *result = zg_client_call_finish(async_result);
    const ZgOpDef *op = zg_op_lookup(run->op_id);
    gboolean ok = (result != NULL && result->ok);

    (void)source;

    /* 认证环节的问题：重新弹窗，而不是当成普通失败 */
    if (result_is_auth_problem(result) && run->attempts < 3)
    {
        gchar *error_text = g_strdup(result->message);

        zg_client_result_free(result);

        zg_dialogs_password(parent, run->op_id, run->args_json, error_text,
                            on_password_ready, run,
                            (GDestroyNotify)run_op_free, run);
        g_free(error_text);
        return;
    }

    if (ok)
    {
        zg_app_log(app, "成功：%s", run->command);

        if (run->success_message != NULL && run->success_message[0] != '\0')
        {
            zg_app_toast(app, "%s", run->success_message);
        }
        else if (result->stdout_text != NULL && result->stdout_text[0] != '\0' &&
                 op != NULL && !op->destructive &&
                 !zg_op_requires_password(op, zg_app_auth_mode(app)))
        {
            zg_dialogs_show_output(parent, op != NULL ? op->title : run->op_id,
                                   result->stdout_text);
        }
        else
        {
            zg_app_toast(app, "操作完成：%s", op != NULL ? op->title : run->op_id);
        }
    }
    else
    {
        GString *detail = g_string_new(NULL);

        if (result != NULL && result->code == ZG_ERR_AUTH_LOCKED)
        {
            g_string_append_printf(detail, "%s\n\n", result->message);
            g_string_append(detail, "为保证安全，请稍后重试或联系系统管理员。");
        }
        else
        {
            g_string_append_printf(detail, "%s\n\n命令：\n%s\n",
                                   (result != NULL && result->message[0] != '\0')
                                           ? result->message
                                           : "命令执行失败",
                                   run->command);

            if (result != NULL && result->stderr_text[0] != '\0')
                g_string_append_printf(detail, "\n错误输出：\n%s", result->stderr_text);

            if (result != NULL && result->stdout_text[0] != '\0')
                g_string_append_printf(detail, "\n标准输出：\n%s", result->stdout_text);
        }

        zg_dialogs_error(parent, "操作失败", detail->str);
        zg_app_log(app, "失败：%s → %s", run->command,
                   result != NULL ? result->message : "后台服务无应答");

        g_string_free(detail, TRUE);
    }

    if (run->done != NULL)
        run->done(ok, run->user_data);

    if (app != NULL)
        zg_app_refresh_all(app);

    zg_client_result_free(result);
    run_op_free(run);
}

void zg_dialogs_run_op(GtkWindow *parent,
                       const gchar *op_id,
                       ZgArgs *args,
                       const gchar *success_message,
                       ZgDialogDoneFunc done,
                       gpointer user_data)
{
    const ZgOpDef *op = zg_op_lookup(op_id);
    gchar *args_json;
    RunOp *run;
    ZgApp *app = zg_app_get_default();

    if (op == NULL)
    {
        zg_dialogs_error(parent, "未知操作", op_id);

        if (args != NULL)
            zg_args_free(args);

        return;
    }

    args_json = zg_args_to_json(args);

    run = g_new0(RunOp, 1);
    run->op_id = g_strdup(op_id);
    run->args_json = args_json;
    run->command = zg_dialogs_describe_args(op_id, args_json);
    run->success_message = g_strdup(success_message);
    run->done = done;
    run->user_data = user_data;

    if (zg_op_requires_password(op, zg_app_auth_mode(app)))
    {
        zg_app_set_status(app, "等待管理员授权：%s", op->title);
        zg_dialogs_password(parent, op_id, args_json, NULL,
                            on_password_ready, run,
                            (GDestroyNotify)run_op_free, run);
        return;
    }

    if (op->destructive)
    {
        gchar *detail = g_strdup_printf(
                "即将执行以下命令：\n\n%s\n\n%s\n\n"
                "该操作不可撤销，请确认目标无误。",
                run->command,
                op->description != NULL ? op->description : "");

        zg_dialogs_confirm(parent, op->title, detail, "确认执行", TRUE,
                           on_run_op_confirmed, run);
        g_free(detail);
        return;
    }

    run_op_execute(run, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* 通用操作表单                                                               */
/* ------------------------------------------------------------------------- */

typedef struct
{
    const ZgOpDef *op;
    GtkWidget *window;
    GtkWidget *preview;
    GtkWidget *positional_box;
    GPtrArray *positional_entries;
    GPtrArray *flag_checks;
    GPtrArray *option_checks;
    GPtrArray *option_entries;
} OpForm;

static void
op_form_free(gpointer data)
{
    OpForm *form = data;

    if (form == NULL)
        return;

    g_ptr_array_free(form->positional_entries, TRUE);
    g_ptr_array_free(form->flag_checks, TRUE);
    g_ptr_array_free(form->option_checks, TRUE);
    g_ptr_array_free(form->option_entries, TRUE);
    g_free(form);
}

static ZgArgs *
op_form_collect(OpForm *form)
{
    ZgArgs *args = zg_args_new();
    guint i;

    if (form->op->spec.flags != NULL)
    {
        for (i = 0; i < form->flag_checks->len; i++)
        {
            GtkWidget *check = g_ptr_array_index(form->flag_checks, i);

            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(check)))
                zg_args_add_flag(args, form->op->spec.flags[i]);
        }
    }

    if (form->op->spec.options != NULL)
    {
        for (i = 0; i < form->option_checks->len; i++)
        {
            GtkWidget *check = g_ptr_array_index(form->option_checks, i);
            GtkWidget *entry = g_ptr_array_index(form->option_entries, i);
            const gchar *text;

            if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(check)))
                continue;

            text = gtk_editable_get_text(GTK_EDITABLE(entry));

            if (text != NULL && text[0] != '\0')
                zg_args_add_option(args, form->op->spec.options[i], text);
        }
    }

    for (i = 0; i < form->positional_entries->len; i++)
    {
        GtkWidget *entry = g_ptr_array_index(form->positional_entries, i);
        const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
        gchar *trimmed = zg_str_trim(text);

        if (trimmed[0] != '\0')
            zg_args_add_positional(args, trimmed);

        g_free(trimmed);
    }

    return args;
}

static void
op_form_update_preview(OpForm *form)
{
    ZgArgs *args = op_form_collect(form);
    gchar *json = zg_args_to_json(args);
    gchar *command = zg_dialogs_describe_args(form->op->id, json);

    gtk_label_set_text(GTK_LABEL(form->preview), command);

    g_free(command);
    g_free(json);
}

static void
on_form_changed(GtkWidget *widget,
                gpointer user_data)
{
    (void)widget;

    op_form_update_preview(user_data);
}

static void
on_option_toggled(GtkCheckButton *check,
                  gpointer user_data)
{
    GtkWidget *entry = g_object_get_data(G_OBJECT(check), "zg-entry");

    (void)user_data;

    if (entry != NULL)
        gtk_widget_set_sensitive(entry, gtk_check_button_get_active(check));
}

static ZgValueKind
positional_kind(const ZgOpDef *op,
                guint index)
{
    if (index < G_N_ELEMENTS(op->spec.kinds))
        return op->spec.kinds[index];

    return op->spec.kinds[G_N_ELEMENTS(op->spec.kinds) - 1];
}

static void
op_form_add_positional_row(OpForm *form,
                           const gchar *value)
{
    const guint index = form->positional_entries->len;
    const ZgValueKind kind = positional_kind(form->op, index);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *entry = gtk_entry_new();
    gchar *hint;

    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
                                   zg_value_kind_label(kind));
    gtk_editable_set_text(GTK_EDITABLE(entry), value != NULL ? value : "");
    gtk_widget_set_hexpand(entry, TRUE);

    hint = g_strdup_printf("参数 %u · %s", index + 1,
                           zg_value_kind_label(kind));

    gtk_box_append(GTK_BOX(row), entry);
    gtk_box_append(GTK_BOX(row), zg_ui_label_dim(hint));
    gtk_box_append(GTK_BOX(form->positional_box), row);

    g_ptr_array_add(form->positional_entries, entry);

    g_signal_connect(entry, "changed", G_CALLBACK(on_form_changed), form);

    g_free(hint);
}

static void
on_add_positional_clicked(GtkButton *button,
                          gpointer user_data)
{
    (void)button;

    op_form_add_positional_row(user_data, NULL);
    op_form_update_preview(user_data);
}

static gboolean
op_form_validate(OpForm *form)
{
    guint i;
    guint filled = 0;

    for (i = 0; i < form->positional_entries->len; i++)
    {
        GtkWidget *entry = g_ptr_array_index(form->positional_entries, i);
        const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
        gchar *trimmed = zg_str_trim(text);
        ZgValueKind kind = positional_kind(form->op, i);

        if (trimmed[0] != '\0')
        {
            filled++;

            if (!zg_value_valid(kind, trimmed))
            {
                gchar *message = g_strdup_printf(
                        "参数 %u 不合法：%s\n\n期望类型：%s",
                        i + 1, trimmed, zg_value_kind_label(kind));

                zg_dialogs_error(GTK_WINDOW(form->window), "参数错误", message);
                g_free(message);
                g_free(trimmed);
                return FALSE;
            }
        }

        g_free(trimmed);
    }

    if (filled < form->op->spec.min_positional)
    {
        gchar *message = g_strdup_printf("该操作至少需要 %u 个参数，当前填写了 %u 个。",
                                         form->op->spec.min_positional, filled);

        zg_dialogs_error(GTK_WINDOW(form->window), "参数不足", message);
        g_free(message);
        return FALSE;
    }

    return TRUE;
}

static void
on_form_execute(GtkButton *button,
                gpointer user_data)
{
    OpForm *form = user_data;
    ZgArgs *args;
    gchar *op_id;

    (void)button;

    if (!op_form_validate(form))
        return;

    args = op_form_collect(form);
    op_id = g_strdup(form->op->id);

    gtk_window_destroy(GTK_WINDOW(form->window));

    zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                      op_id, args, NULL, NULL, NULL);

    g_free(op_id);
}

static GtkWidget *
form_section(GtkWidget *box,
             const gchar *title)
{
    GtkWidget *label = zg_ui_label_heading(title);

    gtk_widget_set_margin_top(label, 6);
    gtk_box_append(GTK_BOX(box), label);

    return label;
}

void zg_dialogs_generic_op(GtkWindow *parent,
                           const gchar *op_id,
                           const gchar *preset1,
                           const gchar *preset2,
                           const gchar *preset3)
{
    const ZgOpDef *op = zg_op_lookup(op_id);
    const gchar *presets[3] = {preset1, preset2, preset3};
    OpForm *form;
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *scrolled;
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gint row = 0;
    guint i;
    guint initial_positionals;

    if (op == NULL)
    {
        zg_dialogs_error(parent, "未知操作", op_id);
        return;
    }

    form = g_new0(OpForm, 1);
    form->op = op;
    form->positional_entries = g_ptr_array_new();
    form->flag_checks = g_ptr_array_new();
    form->option_checks = g_ptr_array_new();
    form->option_entries = g_ptr_array_new();

    window = zg_dialogs_window_new(parent, op->title, 780, 660);
    form->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading(op->title));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(op->description));
    {
        gchar *id_text = g_strdup_printf("%s · 类别：%s%s",
                                         op->id,
                                         zg_op_category_label(op->category),
                                         op->long_running ? " · 可能耗时较长" : "");

        gtk_box_append(GTK_BOX(body), zg_ui_label_dim(id_text));
        g_free(id_text);
    }

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);
    gtk_box_append(GTK_BOX(body), scrolled);

    /* 位置参数 */
    form_section(content, "参数");

    form->positional_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(content), form->positional_box);

    initial_positionals = MAX(op->spec.min_positional, 1);

    if (op->spec.max_positional == 1)
        initial_positionals = 1;
    else if (op->spec.max_positional > 1)
        initial_positionals = MAX(op->spec.min_positional, 1);

    for (i = 0; i < initial_positionals; i++)
        op_form_add_positional_row(form, presets[i]);

    if (op->spec.max_positional == 0)
    {
        GtkWidget *add_button = gtk_button_new_with_label("＋ 添加参数");

        gtk_widget_set_halign(add_button, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(content), add_button);
        g_signal_connect(add_button, "clicked",
                         G_CALLBACK(on_add_positional_clicked), form);
    }

    /* 开关 */
    if (op->spec.flags != NULL && op->spec.flags[0] != NULL)
    {
        GtkWidget *flow = gtk_flow_box_new();

        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);

        for (i = 0; op->spec.flags[i] != NULL; i++)
        {
            GtkWidget *check = gtk_check_button_new_with_label(flag_label(op->spec.flags[i]));

            gtk_flow_box_append(GTK_FLOW_BOX(flow), check);
            g_ptr_array_add(form->flag_checks, check);
            g_signal_connect(check, "toggled", G_CALLBACK(on_form_changed), form);
        }

        form_section(content, "开关");
        gtk_box_append(GTK_BOX(content), flow);
    }

    /* 带值选项 */
    if (op->spec.options != NULL && op->spec.options[0] != NULL)
    {
        grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
        row = 0;

        for (i = 0; op->spec.options[i] != NULL; i++)
        {
            GtkWidget *check = gtk_check_button_new_with_label(option_label(op->spec.options[i]));
            GtkWidget *entry = gtk_entry_new();
            gchar *placeholder = g_strdup_printf("-%s 的取值", op->spec.options[i]);

            gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
            gtk_widget_set_hexpand(entry, TRUE);
            gtk_widget_set_sensitive(entry, FALSE);
            g_signal_connect(check, "toggled", G_CALLBACK(on_form_changed), form);

            gtk_grid_attach(GTK_GRID(grid), check, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);

            g_ptr_array_add(form->option_checks, check);
            g_ptr_array_add(form->option_entries, entry);

            g_free(placeholder);
            row++;
        }

        for (i = 0; i < form->option_checks->len; i++)
        {
            GtkWidget *check = g_ptr_array_index(form->option_checks, i);
            GtkWidget *entry = g_ptr_array_index(form->option_entries, i);

            g_object_set_data(G_OBJECT(check), "zg-entry", entry);
            g_signal_connect(check, "toggled", G_CALLBACK(on_option_toggled), check);
        }

        form_section(content, "带值选项");
        gtk_box_append(GTK_BOX(content), grid);
    }

    /* 命令预览 */
    form_section(content, "将执行的命令");

    form->preview = zg_ui_label_mono("");
    gtk_label_set_wrap(GTK_LABEL(form->preview), TRUE);
    gtk_widget_add_css_class(form->preview, "zg-dim");
    gtk_box_append(GTK_BOX(content), form->preview);

    zg_dialogs_button_row(window, NULL, &accept_button, "执行", op->destructive);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_form_execute), form);

    g_object_set_data_full(G_OBJECT(window), "zg-op-form", form, op_form_free);

    op_form_update_preview(form);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 操作菜单                                                                   */
/* ------------------------------------------------------------------------- */

static void
on_op_menu_row_activated(GtkListBox *list_box,
                         GtkListBoxRow *row,
                         gpointer user_data)
{
    const gchar *op_id = g_object_get_data(G_OBJECT(row), "zg-op-id");
    const gchar *preset1 = g_object_get_data(G_OBJECT(row), "zg-preset1");
    const gchar *preset2 = g_object_get_data(G_OBJECT(row), "zg-preset2");
    GtkWindow *window = user_data;

    (void)list_box;

    gtk_window_destroy(window);
    zg_dialogs_generic_op(zg_app_get_window(zg_app_get_default()),
                          op_id, preset1, preset2, NULL);
}

void zg_dialogs_op_menu(GtkWindow *parent,
                        ZgOpCategory category,
                        const gchar *preset1,
                        const gchar *preset2)
{
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *scrolled;
    GtkWidget *list;
    gchar *title;
    guint i;
    guint count = 0;

    title = g_strdup_printf("%s · 全部操作", zg_op_category_label(category));
    window = zg_dialogs_window_new(parent, title, 760, 620);
    g_free(title);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading(
                                          "选择一个操作打开参数表单；带 ⚠ 标记的操作具有破坏性，会再次确认。"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "提示：表单会实时显示最终执行的命令行，可先核对再执行。"));

    scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
    gtk_box_append(GTK_BOX(body), scrolled);

    for (i = 0; i < zg_op_count(); i++)
    {
        const ZgOpDef *op = zg_op_at(i);
        GtkWidget *row;
        GtkWidget *box;
        GtkWidget *head;
        gchar *head_text;

        if (op->category != category)
            continue;

        count++;

        row = gtk_list_box_row_new();
        box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);

        head_text = g_strdup_printf("%s%s", op->title, op->destructive ? "  ⚠" : "");
        head = zg_ui_label(head_text);
        gtk_box_append(GTK_BOX(box), head);
        gtk_box_append(GTK_BOX(box), zg_ui_label_dim(op->description));

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data(G_OBJECT(row), "zg-op-id", (gpointer)op->id);
        g_object_set_data(G_OBJECT(row), "zg-preset1", (gpointer)preset1);
        g_object_set_data(G_OBJECT(row), "zg-preset2", (gpointer)preset2);

        gtk_list_box_append(GTK_LIST_BOX(list), row);

        g_free(head_text);
    }

    if (count == 0)
        gtk_box_append(GTK_BOX(body), zg_ui_label_dim("该类别暂无操作。"));

    g_signal_connect(list, "row-activated",
                     G_CALLBACK(on_op_menu_row_activated), window);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 通用小工具                                                                 */
/* ------------------------------------------------------------------------- */

/* 按空白与逗号切分设备列表 */
static GPtrArray *
split_tokens(const gchar *text)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    gchar *copy;
    gchar *saveptr = NULL;
    gchar *token;

    if (text == NULL)
        return items;

    copy = g_strdup(text);

    for (token = strtok_r(copy, " \t\n,", &saveptr);
         token != NULL;
         token = strtok_r(NULL, " \t\n,", &saveptr))
    {
        g_ptr_array_add(items, g_strdup(token));
    }

    g_free(copy);

    return items;
}

static GtkWidget *
labeled_field(GtkWidget *grid,
              gint *row,
              const gchar *label,
              GtkWidget *widget,
              const gchar *hint)
{
    zg_dialogs_add_field(grid, row, label, widget, hint);

    return widget;
}

/* ------------------------------------------------------------------------- */
/* 创建存储池                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *name_entry;
    GtkWidget *topology_drop;
    GtkWidget *devices_entry;
    GtkWidget *mountpoint_entry;
    GtkWidget *compression_drop;
    GtkWidget *ashift_drop;
    GtkWidget *force_check;
    GtkWidget *disks_hint;
    GtkWidget *disk_box;
    ZgDialogDoneFunc done;
    gpointer user_data;
} CreatePoolDialog;

static void
on_create_pool_accept(GtkButton *button,
                      gpointer user_data)
{
    CreatePoolDialog *dialog = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gchar *devices_text = gtk_editable_get_text(GTK_EDITABLE(dialog->devices_entry));
    const gchar *mountpoint = gtk_editable_get_text(GTK_EDITABLE(dialog->mountpoint_entry));
    guint topology = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->topology_drop));
    GPtrArray *devices;
    ZgArgs *args;
    guint i;
    guint required = 1;
    gchar *message;

    (void)button;

    if (!zg_pool_name_valid(name))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "池名称非法",
                         "池名称只能包含字母、数字、下划线、点、冒号和连字符，且不能以 '-' 开头。");
        return;
    }

    devices = split_tokens(devices_text);

    if (devices->len == 0)
    {
        g_ptr_array_free(devices, TRUE);
        zg_dialogs_error(GTK_WINDOW(dialog->window), "缺少设备",
                         "请填写至少一个设备路径，例如 /dev/ada1。");
        return;
    }

    for (i = 0; i < devices->len; i++)
    {
        const gchar *device = g_ptr_array_index(devices, i);

        if (!zg_vdev_spec_valid(device))
        {
            message = g_strdup_printf("设备名非法：%s", device);
            zg_dialogs_error(GTK_WINDOW(dialog->window), "设备名非法", message);
            g_free(message);
            g_ptr_array_free(devices, TRUE);
            return;
        }
    }

    switch (topology)
    {
    case 0:
        required = 1;
        break; /* 单盘 */
    case 1:
        required = 2;
        break; /* 条带 */
    case 2:
        required = 2;
        break; /* 镜像 */
    case 3:
        required = 2;
        break; /* RAID-Z1 */
    case 4:
        required = 3;
        break; /* RAID-Z2 */
    case 5:
        required = 4;
        break; /* RAID-Z3 */
    default:
        required = 1;
        break;
    }

    if (topology == 0 && devices->len > 1)
    {
        g_ptr_array_free(devices, TRUE);
        zg_dialogs_error(GTK_WINDOW(dialog->window), "设备数量不符",
                         "单盘模式只能填写一个设备；如需多盘请选择镜像、RAID-Z 或条带。");
        return;
    }

    if (devices->len < required)
    {
        message = g_strdup_printf("所选拓扑至少需要 %u 个设备，当前填写了 %u 个。",
                                  required, devices->len);
        zg_dialogs_error(GTK_WINDOW(dialog->window), "设备数量不足", message);
        g_free(message);
        g_ptr_array_free(devices, TRUE);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, name);

    switch (topology)
    {
    case 2:
        zg_args_add_positional(args, "mirror");
        break;
    case 3:
        zg_args_add_positional(args, "raidz1");
        break;
    case 4:
        zg_args_add_positional(args, "raidz2");
        break;
    case 5:
        zg_args_add_positional(args, "raidz3");
        break;
    default:
        break;
    }

    for (i = 0; i < devices->len; i++)
        zg_args_add_positional(args, g_ptr_array_index(devices, i));

    g_ptr_array_free(devices, TRUE);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->force_check)))
        zg_args_add_flag(args, "-f");

    if (mountpoint[0] != '\0')
        zg_args_add_option(args, "m", mountpoint);

    {
        const guint ashift = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->ashift_drop));

        if (ashift > 0)
        {
            static const gchar *const values[] = {"9", "10", "11", "12", "13", "14", "15", "16"};
            gchar *option = g_strdup_printf("ashift=%s", values[ashift - 1]);

            zg_args_add_option(args, "o", option);
            g_free(option);
        }
    }

    {
        const guint compression = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->compression_drop));

        if (compression > 0)
        {
            static const gchar *const values[] = {
                    "off", "on", "lz4", "zstd", "zstd-3", "gzip-9"};
            gchar *option = g_strdup_printf("compression=%s", values[compression - 1]);

            zg_args_add_option(args, "O", option);
            g_free(option);
        }
    }

    message = g_strdup_printf("存储池 %s 已创建。", name);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->user_data;

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          "zpool.create", args, message, done, done_data);
    }

    g_free(message);
}

typedef struct
{
    GtkWidget **box_slot;
    GtkWidget *entry;
    GPtrArray *disks;
} DiskPickerContext;

/* 粗略判断磁盘是否已被某个 ZFS 池使用（vdev 名字通常是 nda0p3 / da0s1a） */
static gboolean
disk_used_by_pool(const gchar *status_text,
                  const gchar *name)
{
    const gchar *p = status_text;
    gsize len = strlen(name);

    if (status_text == NULL || name == NULL || name[0] == '\0')
        return FALSE;

    while ((p = strstr(p, name)) != NULL)
    {
        gchar after = p[len];

        if (after == '\0' || after == ' ' || after == '\n' || after == '\t' ||
            after == '/' || after == ',' || after == ')' || after == '"' ||
            after == 'p' || after == 's')
            return TRUE;

        p += len;
    }

    return FALSE;
}

static void
on_disk_toggled(GtkCheckButton *check,
                gpointer user_data)
{
    GtkWidget *entry = user_data;
    GtkWidget *box = gtk_widget_get_parent(GTK_WIDGET(check));
    GtkWidget *child;
    GString *text = g_string_new(NULL);

    for (child = gtk_widget_get_first_child(box); child != NULL;
         child = gtk_widget_get_next_sibling(child))
    {
        const gchar *path;

        if (!GTK_IS_CHECK_BUTTON(child) ||
            !gtk_check_button_get_active(GTK_CHECK_BUTTON(child)))
            continue;

        path = g_object_get_data(G_OBJECT(child), "disk-path");

        if (path != NULL)
        {
            if (text->len > 0)
                g_string_append_c(text, ' ');
            g_string_append(text, path);
        }
    }

    gtk_editable_set_text(GTK_EDITABLE(entry), text->str);
    g_string_free(text, TRUE);
}

static void
on_create_pool_status(GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
    DiskPickerContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GtkWidget *box = context->box_slot != NULL ? *context->box_slot : NULL;
    const gchar *status_text = NULL;

    (void)source;

    if (response != NULL && response->ok)
        status_text = response->stdout_text;

    if (box != NULL && context->disks != NULL)
    {
        guint i;

        for (i = 0; i < context->disks->len; i++)
        {
            ZgDisk *disk = g_ptr_array_index(context->disks, i);
            gboolean in_use = disk_used_by_pool(status_text, disk->name);
            gchar *size = zg_format_bytes(disk->size);
            gchar *label = g_strdup_printf(
                    "/dev/%s — %s%s%s%s", disk->name, size,
                    disk->descr != NULL ? "，" : "",
                    disk->descr != NULL ? disk->descr : "",
                    in_use ? "（使用中，禁止选择）" : "");
            GtkWidget *check = gtk_check_button_new_with_label(label);

            if (in_use)
            {
                gtk_widget_set_sensitive(check, FALSE);
                gtk_widget_set_tooltip_text(
                        check, "该磁盘已被 ZFS 池使用（可能包含系统盘），禁止在此选择");
            }
            else
            {
                g_object_set_data_full(G_OBJECT(check), "disk-path",
                                       g_strdup_printf("/dev/%s", disk->name),
                                       g_free);
                g_signal_connect(check, "toggled",
                                 G_CALLBACK(on_disk_toggled), context->entry);
            }

            gtk_box_append(GTK_BOX(box), check);

            g_free(size);
            g_free(label);
        }

        if (context->disks->len == 0)
            gtk_box_append(GTK_BOX(box),
                           zg_ui_label_dim("未检测到磁盘（可运行 geom disk list 查看）"));

        g_ptr_array_free(context->disks, TRUE);
        context->disks = NULL;
    }

    zg_client_result_free(response);
    g_free(context);
}

static void
on_create_pool_disks(GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
    DiskPickerContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
        context->disks = zg_model_disks_from_text(response->stdout_text);
    else
        context->disks = g_ptr_array_new();

    zg_client_result_free(response);

    /* 再取一次池状态，用于屏蔽正在使用的磁盘（含系统盘） */
    zg_client_call_async("zpool.status", NULL, on_create_pool_status, context);
}

void zg_dialogs_create_pool(GtkWindow *parent,
                            ZgDialogDoneFunc done,
                            gpointer data)
{
    static const gchar *const topologies[] = {
            "单盘 (stripe 单个 vdev)", "条带 (多盘无冗余)", "镜像 (mirror)",
            "RAID-Z1 (1 盘冗余)", "RAID-Z2 (2 盘冗余)", "RAID-Z3 (3 盘冗余)", NULL};
    static const gchar *const compressions[] = {
            "继承默认", "off", "on", "lz4", "zstd", "zstd-3", "gzip-9", NULL};
    static const gchar *const ashifts[] = {
            "自动", "9 (512B)", "10 (1K)", "11 (2K)", "12 (4K)", "13 (8K)",
            "14 (16K)", "15 (32K)", "16 (64K)", NULL};
    CreatePoolDialog *dialog = g_new0(CreatePoolDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gint row = 0;

    window = zg_dialogs_window_new(parent, "创建存储池", 720, 560);
    dialog->window = GTK_WIDGET(window);
    dialog->done = done;
    dialog->user_data = data;

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("新建 ZFS 存储池"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "选择拓扑并填写设备。请务必确认设备路径无误 —— 创建池会写入这些设备。"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "可以先用「命令中心 → 存储池 → zpool.create」加 -n 做一次演练。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->name_entry = labeled_field(grid, &row, "池名称",
                                       gtk_entry_new(),
                                       "例如 tank");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->name_entry), "tank");

    dialog->topology_drop = labeled_field(grid, &row, "拓扑",
                                          gtk_drop_down_new_from_strings(topologies),
                                          "镜像/RAID-Z 需要多块设备");

    dialog->devices_entry = labeled_field(grid, &row, "设备列表",
                                          gtk_entry_new(),
                                          "用空格或逗号分隔，例如 /dev/ada1 /dev/ada2");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->devices_entry),
                                   "/dev/ada1 /dev/ada2");
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "检测到以下磁盘，勾选后会自动填入上方「设备列表」；"
                                          "正在被 ZFS 池使用的磁盘（含系统盘）已自动禁用。"));

    dialog->disks_hint = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    dialog->disk_box = dialog->disks_hint;
    gtk_box_append(GTK_BOX(body), dialog->disks_hint);

    {
        GtkWidget **slot = g_new0(GtkWidget *, 1);
        DiskPickerContext *context = g_new0(DiskPickerContext, 1);

        *slot = dialog->disks_hint;
        g_object_add_weak_pointer(G_OBJECT(dialog->disks_hint), (gpointer *)slot);
        context->box_slot = slot;
        context->entry = dialog->devices_entry;
        zg_client_call_async("system.disks", NULL, on_create_pool_disks, context);
    }

    dialog->mountpoint_entry = labeled_field(grid, &row, "挂载点 (-m)",
                                             gtk_entry_new(),
                                             "留空表示使用默认 /池名");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->mountpoint_entry), "/tank");

    dialog->compression_drop = labeled_field(grid, &row, "默认压缩 (-O)",
                                             gtk_drop_down_new_from_strings(compressions),
                                             "为池中新建数据集设置的默认压缩算法");

    dialog->ashift_drop = labeled_field(grid, &row, "扇区大小 ashift",
                                        gtk_drop_down_new_from_strings(ashifts),
                                        "现代硬盘建议 12 (4K)；自动通常也正确");

    dialog->force_check = gtk_check_button_new_with_label("强制创建 (-f，忽略已有标签)");
    gtk_box_append(GTK_BOX(body), dialog->force_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "创建", TRUE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_create_pool_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-create-pool", dialog, g_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 创建数据集 / 卷                                                            */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *kind_drop;
    GtkWidget *name_entry;
    GtkWidget *size_entry;
    GtkWidget *blocksize_entry;
    GtkWidget *compression_drop;
    GtkWidget *quota_entry;
    GtkWidget *reservation_entry;
    GtkWidget *mountpoint_entry;
    GtkWidget *extra_entry;
    GtkWidget *parents_check;
    GtkWidget *nomount_check;
    ZgDialogDoneFunc done;
    gpointer user_data;
} CreateDatasetDialog;

static void
on_create_dataset_accept(GtkButton *button,
                         gpointer user_data)
{
    CreateDatasetDialog *dialog = user_data;
    const gboolean is_volume = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->kind_drop)) == 1;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gchar *size = gtk_editable_get_text(GTK_EDITABLE(dialog->size_entry));
    const gchar *blocksize = gtk_editable_get_text(GTK_EDITABLE(dialog->blocksize_entry));
    const gchar *quota = gtk_editable_get_text(GTK_EDITABLE(dialog->quota_entry));
    const gchar *reservation = gtk_editable_get_text(GTK_EDITABLE(dialog->reservation_entry));
    const gchar *mountpoint = gtk_editable_get_text(GTK_EDITABLE(dialog->mountpoint_entry));
    const gchar *extra = gtk_editable_get_text(GTK_EDITABLE(dialog->extra_entry));
    const guint compression = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->compression_drop));
    ZgArgs *args;
    const gchar *op_id;
    gchar *message;
    gchar **properties = NULL;
    guint i;

    (void)button;

    if (!zg_dataset_name_valid(name))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "数据集名称非法",
                         "名称应形如 tank/data，只能包含字母、数字、下划线、点、冒号和连字符。");
        return;
    }

    if (is_volume && (size == NULL || size[0] == '\0'))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "缺少卷大小",
                         "创建 zvol 必须指定大小，例如 10G。");
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, name);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->parents_check)))
        zg_args_add_flag(args, "-p");

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->nomount_check)))
        zg_args_add_flag(args, "-u");

    if (compression > 0)
    {
        static const gchar *const values[] = {
                "off", "on", "lz4", "zstd", "zstd-3", "gzip-9"};
        gchar *option = g_strdup_printf("compression=%s", values[compression - 1]);

        zg_args_add_option(args, "o", option);
        g_free(option);
    }

    if (quota[0] != '\0')
    {
        gchar *option = g_strdup_printf("quota=%s", quota);

        zg_args_add_option(args, "o", option);
        g_free(option);
    }

    if (reservation[0] != '\0')
    {
        gchar *option = g_strdup_printf("reservation=%s", reservation);

        zg_args_add_option(args, "o", option);
        g_free(option);
    }

    if (mountpoint[0] != '\0')
    {
        gchar *option = g_strdup_printf("mountpoint=%s", mountpoint);

        zg_args_add_option(args, "o", option);
        g_free(option);
    }

    if (extra[0] != '\0')
    {
        properties = g_strsplit_set(extra, ";,", -1);

        for (i = 0; properties[i] != NULL; i++)
        {
            gchar *item = zg_str_trim(properties[i]);
            gchar *equals;

            if (item[0] == '\0')
            {
                g_free(item);
                continue;
            }

            equals = strchr(item, '=');

            if (equals == NULL || equals == item)
            {
                gchar *detail = g_strdup_printf("附加属性格式错误：%s（应为 key=value）", item);

                zg_dialogs_error(GTK_WINDOW(dialog->window), "属性格式错误", detail);
                g_free(detail);
                g_free(item);
                g_strfreev(properties);
                zg_args_free(args);
                return;
            }

            zg_args_add_option(args, "o", item);
            g_free(item);
        }

        g_strfreev(properties);
    }

    if (is_volume)
    {
        op_id = "zfs.create.volume";
        zg_args_add_option(args, "V", size);

        if (blocksize[0] != '\0')
            zg_args_add_option(args, "b", blocksize);
    }
    else
    {
        op_id = "zfs.create";
    }

    message = g_strdup_printf("%s %s 已创建。",
                              is_volume ? "卷" : "数据集", name);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->user_data;
        gchar *op = g_strdup(op_id);

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          op, args, message, done, done_data);

        g_free(op);
    }

    g_free(message);
}

static void
on_dataset_kind_changed(GObject *object,
                        GParamSpec *pspec,
                        gpointer user_data)
{
    CreateDatasetDialog *dialog = user_data;
    const gboolean is_volume =
            gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->kind_drop)) == 1;

    (void)object;
    (void)pspec;

    gtk_widget_set_sensitive(dialog->size_entry, is_volume);
    gtk_widget_set_sensitive(dialog->blocksize_entry, is_volume);
    gtk_widget_set_sensitive(dialog->mountpoint_entry, !is_volume);
}

void zg_dialogs_create_dataset(GtkWindow *parent,
                               const gchar *preset_parent,
                               ZgDialogDoneFunc done,
                               gpointer data)
{
    static const gchar *const kinds[] = {"数据集 (filesystem)", "卷 (zvol)", NULL};
    static const gchar *const compressions[] = {
            "继承默认", "off", "on", "lz4", "zstd", "zstd-3", "gzip-9", NULL};
    CreateDatasetDialog *dialog = g_new0(CreateDatasetDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gchar *preset;
    gint row = 0;

    window = zg_dialogs_window_new(parent, "创建数据集", 720, 620);
    dialog->window = GTK_WIDGET(window);
    dialog->done = done;
    dialog->user_data = data;

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("新建数据集或卷"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "数据集用于文件存储；卷 (zvol) 是块设备，用于 iSCSI、虚拟机磁盘或 swap。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->kind_drop = labeled_field(grid, &row, "类型",
                                      gtk_drop_down_new_from_strings(kinds), NULL);

    dialog->name_entry = labeled_field(grid, &row, "名称", gtk_entry_new(),
                                       "完整路径，例如 tank/data 或 tank/vm/disk0");
    preset = g_strdup_printf("%s/",
                             (preset_parent != NULL && preset_parent[0] != '\0')
                                     ? preset_parent
                                     : "");
    gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), preset);
    gtk_editable_set_position(GTK_EDITABLE(dialog->name_entry), -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->name_entry), "tank/data");
    g_free(preset);

    dialog->size_entry = labeled_field(grid, &row, "卷大小 (-V)", gtk_entry_new(),
                                       "仅卷需要，例如 10G / 512M");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->size_entry), "10G");

    dialog->blocksize_entry = labeled_field(grid, &row, "卷块大小 (-b)", gtk_entry_new(),
                                            "可选，例如 4K / 8K / 64K");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->blocksize_entry), "8K");

    dialog->compression_drop = labeled_field(grid, &row, "压缩",
                                             gtk_drop_down_new_from_strings(compressions),
                                             "lz4 是通用推荐值");

    dialog->quota_entry = labeled_field(grid, &row, "配额 (-o quota=)", gtk_entry_new(),
                                        "留空表示不限制，例如 100G");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->quota_entry), "100G");

    dialog->reservation_entry = labeled_field(grid, &row, "预留 (-o reservation=)", gtk_entry_new(),
                                              "为数据集预留空间，例如 20G");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->reservation_entry), "20G");

    dialog->mountpoint_entry = labeled_field(grid, &row, "挂载点 (-o mountpoint=)", gtk_entry_new(),
                                             "留空表示自动推导；none 表示不挂载");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->mountpoint_entry), "/mnt/data");

    dialog->extra_entry = labeled_field(grid, &row, "附加属性", gtk_entry_new(),
                                        "多个属性用分号分隔，例如 atime=off;recordsize=1M");

    dialog->parents_check = gtk_check_button_new_with_label("自动创建父级数据集 (-p)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->parents_check), TRUE);
    gtk_box_append(GTK_BOX(body), dialog->parents_check);

    dialog->nomount_check = gtk_check_button_new_with_label("创建后不挂载 (-u)");
    gtk_box_append(GTK_BOX(body), dialog->nomount_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "创建", FALSE);
    g_signal_connect(accept_button, "clicked",
                     G_CALLBACK(on_create_dataset_accept), dialog);

    g_signal_connect(dialog->kind_drop, "notify::selected",
                     G_CALLBACK(on_dataset_kind_changed), dialog);
    on_dataset_kind_changed(G_OBJECT(dialog->kind_drop), NULL, dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-create-dataset", dialog, g_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 创建快照                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *dataset_entry;
    GtkWidget *name_entry;
    GtkWidget *recursive_check;
    ZgDialogDoneFunc done;
    gpointer user_data;
} CreateSnapshotDialog;

static void
on_create_snapshot_accept(GtkButton *button,
                          gpointer user_data)
{
    CreateSnapshotDialog *dialog = user_data;
    const gchar *dataset = gtk_editable_get_text(GTK_EDITABLE(dialog->dataset_entry));
    const gchar *snap = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    gchar *target;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (!zg_dataset_name_valid(dataset))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "数据集名称非法",
                         "请填写完整的数据集名称，例如 tank/data。");
        return;
    }

    if (!zg_simple_name_valid(snap))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "快照名称非法",
                         "快照名只能包含字母、数字、下划线、点、冒号和连字符。");
        return;
    }

    target = g_strdup_printf("%s@%s", dataset, snap);

    if (!zg_snapshot_name_valid(target))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "快照名称非法", target);
        g_free(target);
        return;
    }

    args = zg_args_new();
    zg_args_add_positional(args, target);

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->recursive_check)))
        zg_args_add_flag(args, "-r");

    message = g_strdup_printf("快照 %s 已创建。", target);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->user_data;

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          "zfs.snapshot", args, message, done, done_data);
    }

    g_free(message);
    g_free(target);
}

void zg_dialogs_create_snapshot(GtkWindow *parent,
                                const gchar *preset_dataset,
                                ZgDialogDoneFunc done,
                                gpointer data)
{
    CreateSnapshotDialog *dialog = g_new0(CreateSnapshotDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gchar *default_name;
    gint row = 0;

    window = zg_dialogs_window_new(parent, "创建快照", 620, 380);
    dialog->window = GTK_WIDGET(window);
    dialog->done = done;
    dialog->user_data = data;

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("创建快照"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "快照是只读的时间点副本，几乎不占额外空间；可随时回滚或克隆。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->dataset_entry = labeled_field(grid, &row, "数据集", gtk_entry_new(),
                                          "例如 tank/data；递归时会包含全部子级");
    gtk_editable_set_text(GTK_EDITABLE(dialog->dataset_entry),
                          preset_dataset != NULL ? preset_dataset : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->dataset_entry), "tank/data");

    dialog->name_entry = labeled_field(grid, &row, "快照名称", gtk_entry_new(),
                                       "不要包含 @ 符号");

    {
        GDateTime *now = g_date_time_new_now_local();

        default_name = g_date_time_format(now, "auto-%Y%m%d-%H%M%S");
        g_date_time_unref(now);
    }

    gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), default_name);
    gtk_editable_set_position(GTK_EDITABLE(dialog->name_entry), -1);
    g_free(default_name);

    dialog->recursive_check = gtk_check_button_new_with_label("递归创建 (-r，包含全部子数据集)");
    gtk_box_append(GTK_BOX(body), dialog->recursive_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "创建", FALSE);
    g_signal_connect(accept_button, "clicked",
                     G_CALLBACK(on_create_snapshot_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-create-snapshot", dialog, g_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 销毁目标                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    gchar *target;
    gboolean is_pool;
    GtkWidget *recursive_check;
    GtkWidget *force_check;
    GtkWidget *disks_hint;
    GtkWidget *disk_box;
    ZgDialogDoneFunc done;
    gpointer user_data;
} DestroyDialog;

static void
on_destroy_accept(GtkButton *button,
                  gpointer user_data)
{
    DestroyDialog *dialog = user_data;
    ZgArgs *args = zg_args_new();
    gchar *message;

    (void)button;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->force_check)))
        zg_args_add_flag(args, "-f");

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->recursive_check)))
        zg_args_add_flag(args, dialog->is_pool ? "-r" : "-r");

    if (dialog->is_pool)
        zg_args_add_positional(args, dialog->target);
    else
        zg_args_add_positional(args, dialog->target);

    message = g_strdup_printf("%s 已销毁。", dialog->target);

    {
        ZgDialogDoneFunc done = dialog->done;
        gpointer done_data = dialog->user_data;
        const gchar *op = dialog->is_pool ? "zpool.destroy" : "zfs.destroy";

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          op, args, message, done, done_data);
    }

    g_free(message);
}

void zg_dialogs_destroy_target(GtkWindow *parent,
                               const gchar *target,
                               gboolean is_pool,
                               ZgDialogDoneFunc done,
                               gpointer data)
{
    DestroyDialog *dialog = g_new0(DestroyDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *accept_button;
    gchar *detail;

    window = zg_dialogs_window_new(parent, is_pool ? "销毁存储池" : "销毁数据集/快照",
                                   680, 400);
    dialog->window = GTK_WIDGET(window);
    dialog->target = g_strdup(target);
    dialog->is_pool = is_pool;
    dialog->done = done;
    dialog->user_data = data;

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body),
                   zg_ui_label_heading(is_pool ? "销毁存储池" : "销毁数据集或快照"));

    detail = g_strdup_printf(
            "目标：%s\n\n"
            "这将永久删除该对象及其数据，无法通过撤销恢复。\n"
            "如果其中包含快照或克隆，需要相应的递归/强制选项。\n\n"
            "建议先创建快照或确认已有备份。",
            target);

    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(detail));
    g_free(detail);

    dialog->recursive_check = gtk_check_button_new_with_label("递归处理子级 (-r)");
    gtk_box_append(GTK_BOX(body), dialog->recursive_check);

    dialog->force_check = gtk_check_button_new_with_label("强制销毁 (-f)");
    gtk_box_append(GTK_BOX(body), dialog->force_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "销毁", TRUE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_destroy_accept), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-destroy", dialog, g_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 属性编辑器                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    gchar *target;
    gboolean is_pool;
    gchar *property;
    GtkWidget *value_entry;
} PropertyDialog;

static void
on_property_save(GtkButton *button,
                 gpointer user_data)
{
    PropertyDialog *dialog = user_data;
    const gchar *value = gtk_editable_get_text(GTK_EDITABLE(dialog->value_entry));
    ZgArgs *args = zg_args_new();
    gchar *pair;
    gchar *message;

    (void)button;

    if (value == NULL || value[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "属性值不能为空",
                         "如需恢复继承值，请使用「继承」按钮。");
        return;
    }

    pair = g_strdup_printf("%s=%s", dialog->property, value);
    zg_args_add_positional(args, pair);
    zg_args_add_positional(args, dialog->target);

    message = g_strdup_printf("属性 %s 已更新为 %s。", dialog->property, value);

    {
        const gchar *op = dialog->is_pool ? "zpool.set" : "zfs.set";

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                          op, args, message, NULL, NULL);
    }

    g_free(message);
    g_free(pair);
}

static void
on_property_inherit(GtkButton *button,
                    gpointer user_data)
{
    PropertyDialog *dialog = user_data;
    ZgArgs *args = zg_args_new();
    gchar *message;

    (void)button;

    zg_args_add_positional(args, dialog->property);
    zg_args_add_positional(args, dialog->target);

    message = g_strdup_printf("属性 %s 已恢复为继承值。", dialog->property);

    gtk_window_destroy(GTK_WINDOW(dialog->window));
    zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                      "zfs.inherit", args, message, NULL, NULL);

    g_free(message);
}

void zg_dialogs_property_editor(GtkWindow *parent,
                                const gchar *target,
                                gboolean is_pool,
                                const gchar *property,
                                const gchar *value,
                                const gchar *source)
{
    PropertyDialog *dialog = g_new0(PropertyDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *row_box;
    GtkWidget *save_button;
    GtkWidget *inherit_button;
    GtkWidget *cancel_button;
    gchar *title;
    gchar *current;
    gint row = 0;

    title = g_strdup_printf("编辑属性 · %s", property);
    window = zg_dialogs_window_new(parent, title, 640, 340);
    g_free(title);

    dialog->window = GTK_WIDGET(window);
    dialog->target = g_strdup(target);
    dialog->is_pool = is_pool;
    dialog->property = g_strdup(property);

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading(property));

    current = g_strdup_printf("当前值：%s\n来源：%s",
                              (value != NULL && value[0] != '\0') ? value : "—",
                              (source != NULL && source[0] != '\0') ? source : "—");
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(current));
    g_free(current);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->value_entry = labeled_field(grid, &row, "新值", gtk_entry_new(),
                                        "布尔属性用 on/off；大小可写 10G；留空无效");
    gtk_editable_set_text(GTK_EDITABLE(dialog->value_entry),
                          value != NULL ? value : "");
    gtk_editable_set_position(GTK_EDITABLE(dialog->value_entry), -1);

    row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row_box, 8);

    cancel_button = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row_box), cancel_button);

    if (!is_pool)
    {
        inherit_button = gtk_button_new_with_label("恢复继承");
        g_signal_connect(inherit_button, "clicked",
                         G_CALLBACK(on_property_inherit), dialog);
        gtk_box_append(GTK_BOX(row_box), inherit_button);
    }

    save_button = gtk_button_new_with_label("保存");
    gtk_widget_add_css_class(save_button, "suggested-action");
    g_signal_connect(save_button, "clicked", G_CALLBACK(on_property_save), dialog);
    gtk_box_append(GTK_BOX(row_box), save_button);

    gtk_box_append(GTK_BOX(body), row_box);

    g_object_set_data_full(G_OBJECT(window), "zg-property", dialog, g_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 发送快照                                                                   */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* 发送进度窗口                                                               */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *bar;
    GtkWidget *status;
    GtkWidget *close_button;
    GtkTextBuffer *log;
    gchar *target;
    gchar *snapshot;
    GTimer *timer;
    guint tick_source;
    gint refs;
    gboolean dryrun;
} BackupProgress;

static void backup_progress_unref(BackupProgress *progress);

static gchar *
backup_size_text(const gchar *path)
{
    GStatBuf info;

    if (path == NULL || g_stat(path, &info) != 0)
        return g_strdup("0 B");

    return g_format_size((guint64)info.st_size);
}

static void backup_progress_append(BackupProgress *progress, const gchar *format, ...)
        G_GNUC_PRINTF(2, 3);

static void
backup_progress_append(BackupProgress *progress,
                       const gchar *format,
                       ...)
{
    va_list args;
    gchar *text;
    GtkTextIter end;

    va_start(args, format);
    text = g_strdup_vprintf(format, args);
    va_end(args);

    gtk_text_buffer_get_end_iter(progress->log, &end);
    gtk_text_buffer_insert(progress->log, &end, text, -1);
    g_free(text);
}

static void
backup_progress_stop_timer(BackupProgress *progress)
{
    if (progress->tick_source != 0)
    {
        g_source_remove(progress->tick_source);
        progress->tick_source = 0;
    }
}

static gboolean
backup_progress_tick(gpointer data)
{
    BackupProgress *progress = data;
    gdouble elapsed = g_timer_elapsed(progress->timer, NULL);
    gchar *written;
    gchar *text;

    if (progress->window == NULL)
        return G_SOURCE_REMOVE;

    written = backup_size_text(progress->target);
    text = g_strdup_printf("已写入 %s · 用时 %.0f 秒", written, elapsed);
    gtk_label_set_text(GTK_LABEL(progress->status), text);
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress->bar));

    g_free(text);
    g_free(written);

    return G_SOURCE_CONTINUE;
}

static void
backup_window_destroyed(gpointer data)
{
    BackupProgress *progress = data;

    progress->window = NULL;
    backup_progress_stop_timer(progress);
    backup_progress_unref(progress);
}

static void
backup_progress_unref(BackupProgress *progress)
{
    if (!g_atomic_int_dec_and_test(&progress->refs))
        return;

    backup_progress_stop_timer(progress);

    if (progress->timer != NULL)
        g_timer_destroy(progress->timer);

    g_free(progress->target);
    g_free(progress->snapshot);
    g_free(progress);
}

static void
on_backup_done(GObject *source,
               GAsyncResult *result,
               gpointer user_data)
{
    BackupProgress *progress = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    gboolean ok = response != NULL && response->ok;
    gdouble elapsed = g_timer_elapsed(progress->timer, NULL);
    gchar *size;
    gchar *text;

    (void)source;

    backup_progress_stop_timer(progress);
    size = backup_size_text(progress->target);

    if (progress->window != NULL)
    {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress->bar), ok ? 1.0 : 0.0);

        if (ok && progress->dryrun)
            text = g_strdup_printf("演练完成（未写出数据）· 用时 %.0f 秒", elapsed);
        else if (ok)
            text = g_strdup_printf("备份完成 · 共 %s · 用时 %.0f 秒", size, elapsed);
        else
            text = g_strdup_printf("备份失败 · 用时 %.0f 秒", elapsed);

        gtk_label_set_text(GTK_LABEL(progress->status), text);
        g_free(text);

        backup_progress_append(progress, "\n%s\n%s\n",
                               ok ? "== 完成 ==" : "== 失败 ==",
                               ok ? ((response->stdout_text[0] != '\0')
                                             ? response->stdout_text
                                             : "数据已写入目标文件")
                                  : ((response->message[0] != '\0')
                                             ? response->message
                                             : "后台服务无应答"));

        gtk_widget_set_sensitive(progress->close_button, TRUE);
        gtk_window_set_deletable(GTK_WINDOW(progress->window), TRUE);
    }

    if (ok)
    {
        zg_app_toast(zg_app_get_default(), "快照已备份到 %s", progress->target);
        zg_app_refresh_all(zg_app_get_default());
    }
    else
    {
        zg_app_log(zg_app_get_default(), "备份失败：%s",
                   response != NULL ? response->message : "无应答");
    }

    g_free(size);
    zg_client_result_free(response);
    backup_progress_unref(progress);
}

static void
backup_progress_start(GtkWindow *parent,
                      const gchar *snapshot,
                      const gchar *target,
                      gchar *args_json,
                      gboolean dryrun)
{
    BackupProgress *progress = g_new0(BackupProgress, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *scrolled;
    GtkWidget *close_row;
    GtkWidget *close_button;
    gint row = 0;

    progress->target = g_strdup(target);
    progress->snapshot = g_strdup(snapshot);
    progress->timer = g_timer_new();
    progress->dryrun = dryrun;
    progress->refs = 2; /* 窗口 + 异步调用 */

    window = zg_dialogs_window_new(parent, "快照备份", 760, 540);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_deletable(GTK_WINDOW(window), FALSE);
    progress->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("正在备份快照"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "备份在后台执行；下面的进度会实时更新，完成后可关闭窗口。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    zg_dialogs_add_field(grid, &row, "快照", zg_ui_label(snapshot), NULL);
    {
        GtkWidget *target_label = zg_ui_label(target);

        gtk_label_set_wrap(GTK_LABEL(target_label), TRUE);
        gtk_label_set_selectable(GTK_LABEL(target_label), TRUE);
        gtk_widget_add_css_class(target_label, "zg-mono");
        zg_dialogs_add_field(grid, &row, "目标文件", target_label, NULL);
    }

    progress->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(progress->bar), 0.06);
    gtk_box_append(GTK_BOX(body), progress->bar);

    progress->status = zg_ui_label("准备中…");
    gtk_widget_add_css_class(progress->status, "zg-cell-bold");
    gtk_box_append(GTK_BOX(body), progress->status);

    scrolled = zg_ui_text_view_new(&progress->log, FALSE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(body), scrolled);

    close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(close_row, GTK_ALIGN_END);
    close_button = gtk_button_new_with_label("关闭");
    progress->close_button = close_button;
    gtk_widget_add_css_class(close_button, "suggested-action");
    gtk_widget_set_sensitive(close_button, FALSE);
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_close), window);
    gtk_box_append(GTK_BOX(close_row), close_button);
    gtk_box_append(GTK_BOX(body), close_row);

    g_object_set_data_full(G_OBJECT(window), "zg-backup", progress,
                           backup_window_destroyed);

    backup_progress_append(progress, "开始时间：");
    {
        GDateTime *now = g_date_time_new_now_local();
        gchar *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");

        backup_progress_append(progress, "%s\n", stamp);
        g_free(stamp);
        g_date_time_unref(now);
    }
    backup_progress_append(progress, "快照：%s\n目标：%s\n\n", snapshot, target);

    progress->tick_source = g_timeout_add(500, backup_progress_tick, progress);
    zg_client_call_async("zfs.send", args_json, on_backup_done, progress);

    gtk_window_present(window);
}

typedef struct
{
    GtkWidget *window;
    GtkWidget *snapshot_entry;
    GtkWidget *output_entry;
    GtkWidget *mode_drop;
    GtkWidget *base_entry;
    GtkWidget *compress_check;
    GtkWidget *dryrun_check;
    ZgDialogDoneFunc done;
    gpointer user_data;
} SendDialog;

static void
on_send_mode_changed(GObject *object,
                     GParamSpec *pspec,
                     gpointer user_data)
{
    SendDialog *dialog = user_data;
    const guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->mode_drop));

    (void)object;
    (void)pspec;

    gtk_widget_set_sensitive(dialog->base_entry, mode == 1 || mode == 3);
}

static gchar *
default_backup_name(const gchar *snapshot)
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

/* ------------------------------------------------------------------------- */
/* 系统文件选择器                                                             */
/* ------------------------------------------------------------------------- */

static void
on_pick_save_done(GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GtkWidget *entry = user_data;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, NULL);

    if (file != NULL)
    {
        gchar *path = g_file_get_path(file);

        if (path != NULL)
            gtk_editable_set_text(GTK_EDITABLE(entry), path);

        g_free(path);
        g_object_unref(file);
    }
}

static void
on_pick_open_done(GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GtkWidget *entry = user_data;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, NULL);

    if (file != NULL)
    {
        gchar *path = g_file_get_path(file);

        if (path != NULL)
            gtk_editable_set_text(GTK_EDITABLE(entry), path);

        g_free(path);
        g_object_unref(file);
    }
}

void zg_dialogs_pick_save(GtkWindow *parent,
                          GtkWidget *entry,
                          const gchar *suggested_name)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();

    gtk_file_dialog_set_title(dialog, "选择保存位置");
    if (suggested_name != NULL)
        gtk_file_dialog_set_initial_name(dialog, suggested_name);
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_save(dialog, parent, NULL, on_pick_save_done, entry);
    g_object_unref(dialog);
}

void zg_dialogs_pick_open(GtkWindow *parent,
                          GtkWidget *entry)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();

    gtk_file_dialog_set_title(dialog, "选择文件");
    gtk_file_dialog_set_modal(dialog, TRUE);
    gtk_file_dialog_open(dialog, parent, NULL, on_pick_open_done, entry);
    g_object_unref(dialog);
}

static void
on_send_browse(GtkButton *button,
               gpointer user_data)
{
    SendDialog *dialog = user_data;
    const gchar *snapshot = gtk_editable_get_text(GTK_EDITABLE(dialog->snapshot_entry));
    gchar *name = default_backup_name(snapshot);

    (void)button;

    zg_dialogs_pick_save(GTK_WINDOW(dialog->window), dialog->output_entry, name);
    g_free(name);
}

static void
on_send_accept(GtkButton *button,
               gpointer user_data)
{
    SendDialog *dialog = user_data;
    const gchar *snapshot = gtk_editable_get_text(GTK_EDITABLE(dialog->snapshot_entry));
    const gchar *output = gtk_editable_get_text(GTK_EDITABLE(dialog->output_entry));
    const gchar *base = gtk_editable_get_text(GTK_EDITABLE(dialog->base_entry));
    const guint mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->mode_drop));
    ZgArgs *args;

    (void)button;

    if (!zg_snapshot_name_valid(snapshot))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "快照名称非法",
                         "请填写完整快照名，例如 tank/data@backup1。");
        return;
    }

    if (output == NULL || output[0] != '/')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "输出路径非法",
                         "请填写绝对路径，例如 /backup/tank-data.zfs。");
        return;
    }

    if ((mode == 1 || mode == 3) && !zg_snapshot_name_valid(base))
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "增量起点非法",
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

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->compress_check)))
        zg_args_add_flag(args, "-c");

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->dryrun_check)))
        zg_args_add_flag(args, "-n");

    zg_args_set_io(args, "output", output);

    {
        gboolean dryrun = gtk_check_button_get_active(
                GTK_CHECK_BUTTON(dialog->dryrun_check));
        gchar *args_json = zg_args_to_json(args);
        gchar *snapshot_copy = g_strdup(snapshot);
        gchar *output_copy = g_strdup(output);

        gtk_window_destroy(GTK_WINDOW(dialog->window));
        backup_progress_start(zg_app_get_window(zg_app_get_default()),
                              snapshot_copy, output_copy, args_json, dryrun);

        g_free(snapshot_copy);
        g_free(output_copy);
    }
}

void zg_dialogs_send_snapshot(GtkWindow *parent,
                              const gchar *snapshot,
                              ZgDialogDoneFunc done,
                              gpointer data)
{
    static const gchar *const modes[] = {
            "全量发送", "增量 (-i，需起始快照)", "复制 (-R，含属性与后代)", "增量含中间快照 (-I)",
            NULL};
    SendDialog *dialog = g_new0(SendDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *accept_button;
    gint row = 0;

    window = zg_dialogs_window_new(parent, "发送快照流", 720, 460);
    dialog->window = GTK_WIDGET(window);
    dialog->done = done;
    dialog->user_data = data;

    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("发送快照到文件"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "生成可直接分发或归档的 ZFS 快照流；接收端用 zfs receive 还原。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->snapshot_entry = labeled_field(grid, &row, "源快照", gtk_entry_new(),
                                           "dataset@snapshot");
    gtk_editable_set_text(GTK_EDITABLE(dialog->snapshot_entry),
                          snapshot != NULL ? snapshot : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->snapshot_entry), "tank/data@backup1");

    {
        GtkWidget *output_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *browse = gtk_button_new_with_label("浏览…");

        dialog->output_entry = gtk_entry_new();
        gtk_widget_set_hexpand(dialog->output_entry, TRUE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->output_entry),
                                       "/backup/tank-data.zfs");
        gtk_box_append(GTK_BOX(output_row), dialog->output_entry);
        gtk_box_append(GTK_BOX(output_row), browse);
        g_signal_connect(browse, "clicked", G_CALLBACK(on_send_browse), dialog);

        labeled_field(grid, &row, "输出文件", output_row,
                      "选择保存位置；可保存到本机目录或已挂载的外接硬盘");
    }

    dialog->mode_drop = labeled_field(grid, &row, "发送模式",
                                      gtk_drop_down_new_from_strings(modes), NULL);

    dialog->base_entry = labeled_field(grid, &row, "起始快照", gtk_entry_new(),
                                       "仅增量模式需要");
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->base_entry), "tank/data@base");
    gtk_widget_set_sensitive(dialog->base_entry, FALSE);

    dialog->compress_check = gtk_check_button_new_with_label("压缩流 (-c)");
    gtk_box_append(GTK_BOX(body), dialog->compress_check);

    dialog->dryrun_check = gtk_check_button_new_with_label("仅演练，不写出数据 (-n)");
    gtk_box_append(GTK_BOX(body), dialog->dryrun_check);

    zg_dialogs_button_row(window, NULL, &accept_button, "开始发送", FALSE);
    g_signal_connect(accept_button, "clicked", G_CALLBACK(on_send_accept), dialog);

    g_signal_connect(dialog->mode_drop, "notify::selected",
                     G_CALLBACK(on_send_mode_changed), dialog);

    g_object_set_data_full(G_OBJECT(window), "zg-send", dialog, g_free);

    gtk_window_present(window);
}
