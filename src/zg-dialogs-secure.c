/* zg-dialogs-secure.c - 加密 / 委派 / 配额 / 共享 对话框
 *
 * 全部为语义化表单：用户选择"谁、什么权限、多少配额"，
 * 由界面拼装成正确的 ZFS 命令。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-ui.h"
#include "zg-util.h"

/* ------------------------------------------------------------------------- */
/* 公共：执行并关闭窗口                                                       */
/* ------------------------------------------------------------------------- */

static void
run_and_close(GtkWidget *window,
              const gchar *op_id,
              ZgArgs *args,
              const gchar *message)
{
    gtk_window_destroy(GTK_WINDOW(window));
    zg_dialogs_run_op(zg_app_get_window(zg_app_get_default()),
                      op_id, args, message, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* 加密与密钥                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *keylocation;
    gchar *dataset;
} EncryptDialog;

static void
encrypt_dialog_free(gpointer data)
{
    EncryptDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->dataset);
    g_free(dialog);
}

static void
on_load_key(GtkButton *button,
            gpointer user_data)
{
    EncryptDialog *dialog = user_data;
    const gchar *location =
            gtk_editable_get_text(GTK_EDITABLE(dialog->keylocation));
    ZgArgs *args = zg_args_new();
    gchar *message;

    (void)button;

    zg_args_add_positional(args, dialog->dataset);

    if (location != NULL && location[0] != '\0')
        zg_args_add_option(args, "L", location);

    message = g_strdup_printf("密钥已加载，数据集 %s 现在可访问。", dialog->dataset);

    run_and_close(dialog->window, "zfs.load-key", args, message);
    g_free(message);
}

static void
on_unload_key(GtkButton *button,
              gpointer user_data)
{
    EncryptDialog *dialog = user_data;
    ZgArgs *args = zg_args_new();
    gchar *message;

    (void)button;

    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("密钥已卸载，数据集 %s 已锁定。", dialog->dataset);

    run_and_close(dialog->window, "zfs.unload-key", args, message);
    g_free(message);
}

static void
on_change_key(GtkButton *button,
              gpointer user_data)
{
    EncryptDialog *dialog = user_data;
    ZgArgs *args = zg_args_new();
    gchar *message;

    (void)button;

    zg_args_add_flag(args, "-i");
    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("数据集 %s 的密钥已重新生成。", dialog->dataset);

    run_and_close(dialog->window, "zfs.change-key", args, message);
    g_free(message);
}

void zg_dialogs_encryption(GtkWindow *parent,
                           const gchar *dataset,
                           const gchar *keystatus)
{
    EncryptDialog *dialog = g_new0(EncryptDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *row;
    GtkWidget *cancel_button;
    GtkWidget *button;
    gchar *status_text;
    gint grid_row = 0;

    dialog->dataset = g_strdup(dataset);

    window = zg_dialogs_window_new(parent, "加密与密钥", 680, 480);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("加密数据集密钥"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(dataset));

    status_text = g_strdup_printf("当前密钥状态：%s",
                                  (keystatus != NULL && keystatus[0] != '\0' &&
                                   g_strcmp0(keystatus, "-") != 0)
                                          ? keystatus
                                          : "未知");
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(status_text));
    g_free(status_text);

    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "加载密钥后才能访问加密数据；卸载后数据保持锁定但仍可发送快照。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->keylocation = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->keylocation),
                                   "留空表示使用数据集默认的 keylocation");
    zg_dialogs_add_field(grid, &grid_row, "密钥位置", dialog->keylocation,
                         "例如 file:///root/keys/tank.key");

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 8);

    cancel_button = gtk_button_new_with_label("关闭");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel_button);

    button = zg_ui_button("更换密钥", G_CALLBACK(on_change_key), dialog);
    gtk_box_append(GTK_BOX(row), button);

    button = zg_ui_button("卸载密钥", G_CALLBACK(on_unload_key), dialog);
    gtk_box_append(GTK_BOX(row), button);

    button = zg_ui_button("加载密钥", G_CALLBACK(on_load_key), dialog);
    gtk_widget_add_css_class(button, "suggested-action");
    gtk_box_append(GTK_BOX(row), button);

    gtk_box_append(GTK_BOX(body), row);

    g_object_set_data_full(G_OBJECT(window), "zg-encrypt", dialog,
                           encrypt_dialog_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 委派权限                                                                   */
/* ------------------------------------------------------------------------- */

static const struct
{
    const gchar *permission;
    const gchar *label;
    gboolean active;
} delegation_permissions[] = {
        {"create", "创建数据集 create", TRUE},
        {"destroy", "销毁数据集 destroy", FALSE},
        {"snapshot", "创建快照 snapshot", TRUE},
        {"rollback", "回滚 rollback", FALSE},
        {"clone", "克隆 clone", FALSE},
        {"promote", "提升 promote", FALSE},
        {"rename", "重命名 rename", FALSE},
        {"mount", "挂载 mount", TRUE},
        {"share", "共享 share", FALSE},
        {"send", "发送 send", TRUE},
        {"receive", "接收 receive", FALSE},
        {"hold", "持有快照 hold", FALSE},
        {"release", "释放快照 release", FALSE},
        {"diff", "差异 diff", FALSE},
        {"userprop", "用户属性 userprop", FALSE},
        {"readonly", "只读访问 readonly", FALSE},
};

typedef struct
{
    GtkWidget *window;
    GtkWidget *kind_drop;
    GtkWidget *name_entry;
    GtkWidget **checks;
    gchar *dataset;
    guint count;
} DelegationDialog;

static void
delegation_dialog_free(gpointer data)
{
    DelegationDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->dataset);
    g_free(dialog->checks);
    g_free(dialog);
}

static gchar *
delegation_permission_list(DelegationDialog *dialog)
{
    GString *text = g_string_new(NULL);
    guint i;

    for (i = 0; i < dialog->count; i++)
    {
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->checks[i])))
            continue;

        if (text->len > 0)
            g_string_append_c(text, ',');

        g_string_append(text, delegation_permissions[i].permission);
    }

    return g_string_free(text, FALSE);
}

static void
on_delegation_grant(GtkButton *button,
                    gpointer user_data)
{
    DelegationDialog *dialog = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gboolean is_group =
            gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->kind_drop)) == 1;
    gchar *permissions = delegation_permission_list(dialog);
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (name == NULL || name[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请输入用户或组",
                         "需要指定被授权的用户或组名。");
        g_free(permissions);
        return;
    }

    if (permissions[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请选择权限",
                         "至少勾选一项要授予的权限。");
        g_free(permissions);
        return;
    }

    args = zg_args_new();
    zg_args_add_flag(args, is_group ? "-g" : "-u");
    zg_args_add_positional(args, name);
    zg_args_add_positional(args, permissions);
    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("已向 %s%s 授予权限：%s",
                              is_group ? "组 " : "用户 ", name, permissions);

    run_and_close(dialog->window, "zfs.allow", args, message);

    g_free(message);
    g_free(permissions);
}

static void
on_delegation_revoke(GtkButton *button,
                     gpointer user_data)
{
    DelegationDialog *dialog = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gboolean is_group =
            gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->kind_drop)) == 1;
    ZgArgs *args;
    gchar *message;

    (void)button;

    if (name == NULL || name[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请输入用户或组",
                         "需要指定要撤销授权的用户或组名。");
        return;
    }

    args = zg_args_new();
    zg_args_add_flag(args, is_group ? "-g" : "-u");
    zg_args_add_positional(args, name);
    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("已撤销 %s%s 在 %s 上的全部委派权限。",
                              is_group ? "组 " : "用户 ", name, dialog->dataset);

    run_and_close(dialog->window, "zfs.unallow", args, message);
    g_free(message);
}

void zg_dialogs_delegation(GtkWindow *parent,
                           const gchar *dataset)
{
    static const gchar *const kinds[] = {"用户", "组", NULL};
    DelegationDialog *dialog = g_new0(DelegationDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *flow;
    GtkWidget *row;
    GtkWidget *cancel_button;
    GtkWidget *button;
    gint grid_row = 0;
    guint i;

    dialog->dataset = g_strdup(dataset);
    dialog->count = G_N_ELEMENTS(delegation_permissions);
    dialog->checks = g_new0(GtkWidget *, dialog->count);

    window = zg_dialogs_window_new(parent, "委派权限", 760, 640);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("把管理权限委派给普通用户"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(dataset));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "被授权用户无需 root 即可在指定数据集上执行勾选的操作。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->kind_drop = gtk_drop_down_new_from_strings(kinds);
    zg_dialogs_add_field(grid, &grid_row, "类型", dialog->kind_drop, NULL);

    dialog->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->name_entry), "用户名或组名");
    zg_dialogs_add_field(grid, &grid_row, "名称", dialog->name_entry, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("权限"));

    flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);

    for (i = 0; i < dialog->count; i++)
    {
        GtkWidget *check = gtk_check_button_new_with_label(delegation_permissions[i].label);

        gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
                                    delegation_permissions[i].active);
        dialog->checks[i] = check;
        gtk_flow_box_append(GTK_FLOW_BOX(flow), check);
    }

    gtk_box_append(GTK_BOX(body), flow);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 8);

    cancel_button = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel_button);

    button = zg_ui_button("撤销授权", G_CALLBACK(on_delegation_revoke), dialog);
    gtk_box_append(GTK_BOX(row), button);

    button = zg_ui_button("授予权限", G_CALLBACK(on_delegation_grant), dialog);
    gtk_widget_add_css_class(button, "suggested-action");
    gtk_box_append(GTK_BOX(row), button);

    gtk_box_append(GTK_BOX(body), row);

    g_object_set_data_full(G_OBJECT(window), "zg-delegation", dialog,
                           delegation_dialog_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 用户 / 组配额                                                              */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *kind_drop;
    GtkWidget *name_entry;
    GtkWidget *quota_entry;
    gchar *dataset;
} QuotaDialog;

static void
quota_dialog_free(gpointer data)
{
    QuotaDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->dataset);
    g_free(dialog);
}

static void
on_quota_apply(GtkButton *button,
               gpointer user_data)
{
    QuotaDialog *dialog = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gchar *quota = gtk_editable_get_text(GTK_EDITABLE(dialog->quota_entry));
    const gboolean is_group =
            gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->kind_drop)) == 1;
    ZgArgs *args;
    gchar *property;
    gchar *message;

    (void)button;

    if (name == NULL || name[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请输入用户或组",
                         "需要指定要设置配额的用户或组。");
        return;
    }

    if (quota == NULL || quota[0] == '\0')
    {
        zg_dialogs_error(GTK_WINDOW(dialog->window), "请输入配额",
                         "例如 50G；填写 none 表示取消配额。");
        return;
    }

    property = g_strdup_printf("%squota@%s=%s",
                               is_group ? "group" : "user",
                               name, quota);

    args = zg_args_new();
    zg_args_add_positional(args, property);
    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("已为 %s%s 设置配额 %s。",
                              is_group ? "组 " : "用户 ", name, quota);

    run_and_close(dialog->window, "zfs.set", args, message);

    g_free(message);
    g_free(property);
}

void zg_dialogs_quota(GtkWindow *parent,
                      const gchar *dataset)
{
    static const gchar *const kinds[] = {"用户", "组", NULL};
    QuotaDialog *dialog = g_new0(QuotaDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *grid;
    GtkWidget *row;
    GtkWidget *cancel_button;
    GtkWidget *apply_button;
    gint grid_row = 0;

    dialog->dataset = g_strdup(dataset);

    window = zg_dialogs_window_new(parent, "用户与组配额", 680, 460);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("设置用户 / 组空间配额"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(dataset));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "配额限制某个用户或组在该数据集下可使用的空间，不影响其他用户。"));

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_box_append(GTK_BOX(body), grid);

    dialog->kind_drop = gtk_drop_down_new_from_strings(kinds);
    zg_dialogs_add_field(grid, &grid_row, "类型", dialog->kind_drop, NULL);

    dialog->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->name_entry), "用户名或组名");
    zg_dialogs_add_field(grid, &grid_row, "名称", dialog->name_entry, NULL);

    dialog->quota_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dialog->quota_entry), "例如 50G，none 表示取消");
    zg_dialogs_add_field(grid, &grid_row, "配额", dialog->quota_entry,
                         "支持 K/M/G/T 后缀");

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 8);

    cancel_button = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel_button);

    apply_button = gtk_button_new_with_label("应用配额");
    gtk_widget_add_css_class(apply_button, "suggested-action");
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_quota_apply), dialog);
    gtk_box_append(GTK_BOX(row), apply_button);

    gtk_box_append(GTK_BOX(body), row);

    g_object_set_data_full(G_OBJECT(window), "zg-quota", dialog,
                           quota_dialog_free);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* NFS / SMB 共享                                                             */
/* ------------------------------------------------------------------------- */

typedef struct
{
    GtkWidget *window;
    GtkWidget *nfs_check;
    GtkWidget *smb_check;
    gchar *dataset;
} ShareDialog;

static void
share_dialog_free(gpointer data)
{
    ShareDialog *dialog = data;

    if (dialog == NULL)
        return;

    g_free(dialog->dataset);
    g_free(dialog);
}

static void
on_share_apply(GtkButton *button,
               gpointer user_data)
{
    ShareDialog *dialog = user_data;
    const gboolean nfs = gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->nfs_check));
    const gboolean smb = gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->smb_check));
    ZgArgs *args = zg_args_new();
    gchar *nfs_property = g_strdup_printf("sharenfs=%s", nfs ? "on" : "off");
    gchar *smb_property = g_strdup_printf("sharesmb=%s", smb ? "on" : "off");
    gchar *message;

    (void)button;

    zg_args_add_positional(args, nfs_property);
    zg_args_add_positional(args, smb_property);
    zg_args_add_positional(args, dialog->dataset);

    message = g_strdup_printf("共享设置已更新：NFS %s，SMB %s。",
                              nfs ? "开启" : "关闭", smb ? "开启" : "关闭");

    run_and_close(dialog->window, "zfs.set", args, message);

    g_free(message);
    g_free(nfs_property);
    g_free(smb_property);
}

void zg_dialogs_share(GtkWindow *parent,
                      const gchar *dataset)
{
    ShareDialog *dialog = g_new0(ShareDialog, 1);
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *row;
    GtkWidget *cancel_button;
    GtkWidget *apply_button;

    dialog->dataset = g_strdup(dataset);

    window = zg_dialogs_window_new(parent, "共享设置", 640, 400);
    dialog->window = GTK_WIDGET(window);

    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("通过网络共享数据集"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_wrap(dataset));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "开启后由 ZFS 自动管理 NFS / SMB 导出，与 /etc/exports 无关。"));

    dialog->nfs_check = gtk_check_button_new_with_label("通过 NFS 共享");
    dialog->smb_check = gtk_check_button_new_with_label("通过 SMB 共享");
    gtk_box_append(GTK_BOX(body), dialog->nfs_check);
    gtk_box_append(GTK_BOX(body), dialog->smb_check);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 8);

    cancel_button = gtk_button_new_with_label("取消");
    g_signal_connect_swapped(cancel_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), window);
    gtk_box_append(GTK_BOX(row), cancel_button);

    apply_button = gtk_button_new_with_label("应用");
    gtk_widget_add_css_class(apply_button, "suggested-action");
    g_signal_connect(apply_button, "clicked", G_CALLBACK(on_share_apply), dialog);
    gtk_box_append(GTK_BOX(row), apply_button);

    gtk_box_append(GTK_BOX(body), row);

    g_object_set_data_full(G_OBJECT(window), "zg-share", dialog,
                           share_dialog_free);

    gtk_window_present(window);
}
