/* zg-dialogs.h - 对话框与操作执行
 *
 * 所有破坏性操作都会在此弹出确认框，并显示即将执行的完整命令。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_DIALOGS_H
#define ZG_DIALOGS_H

#include <gtk/gtk.h>

#include "zg-client.h"
#include "zg-opdef.h"

G_BEGIN_DECLS

typedef void (*ZgConfirmFunc)(gpointer data);
typedef void (*ZgDialogDoneFunc)(gboolean success, gpointer data);

/* 提示框 */
void zg_dialogs_error(GtkWindow *parent,
                      const gchar *title,
                      const gchar *detail);
void zg_dialogs_info(GtkWindow *parent,
                     const gchar *title,
                     const gchar *detail);
void zg_dialogs_confirm(GtkWindow *parent,
                        const gchar *title,
                        const gchar *detail,
                        const gchar *accept_label,
                        gboolean destructive,
                        ZgConfirmFunc on_accept,
                        gpointer data);
void zg_dialogs_show_output(GtkWindow *parent,
                            const gchar *title,
                            const gchar *text);

/* 执行注册表中的操作；args 的所有权被接管。
 * success_message 为 NULL 时，只读操作的输出会直接以窗口展示。 */
void zg_dialogs_run_op(GtkWindow *parent,
                       const gchar *op_id,
                       ZgArgs *args,
                       const gchar *success_message,
                       ZgDialogDoneFunc done,
                       gpointer user_data);

/* 通用表单：根据注册表自动生成输入控件 */
void zg_dialogs_generic_op(GtkWindow *parent,
                           const gchar *op_id,
                           const gchar *preset1,
                           const gchar *preset2,
                           const gchar *preset3);

/* 按类别列出全部操作，供「更多…」使用 */
void zg_dialogs_op_menu(GtkWindow *parent,
                        ZgOpCategory category,
                        const gchar *preset1,
                        const gchar *preset2);

/* 常用向导 */
void zg_dialogs_create_pool(GtkWindow *parent,
                            ZgDialogDoneFunc done,
                            gpointer data);
void zg_dialogs_create_dataset(GtkWindow *parent,
                               const gchar *preset_parent,
                               ZgDialogDoneFunc done,
                               gpointer data);
void zg_dialogs_create_snapshot(GtkWindow *parent,
                                const gchar *preset_dataset,
                                ZgDialogDoneFunc done,
                                gpointer data);
void zg_dialogs_destroy_target(GtkWindow *parent,
                               const gchar *target,
                               gboolean is_pool,
                               ZgDialogDoneFunc done,
                               gpointer data);
void zg_dialogs_property_editor(GtkWindow *parent,
                                const gchar *target,
                                gboolean is_pool,
                                const gchar *property,
                                const gchar *value,
                                const gchar *source);
void zg_dialogs_send_snapshot(GtkWindow *parent,
                              const gchar *snapshot,
                              ZgDialogDoneFunc done,
                              gpointer data);

/* 由页面模块复用的工具函数 */
gchar *zg_dialogs_describe_args(const gchar *op_id,
                                const gchar *args_json);
GtkWindow *zg_dialogs_window_new(GtkWindow *parent,
                                 const gchar *title,
                                 gint width,
                                 gint height);
GtkWidget *zg_dialogs_body(GtkWindow *window,
                           GtkWidget **out_box);
GtkWidget *zg_dialogs_button_row(GtkWindow *window,
                                 GtkWidget **out_cancel,
                                 GtkWidget **out_accept,
                                 const gchar *accept_label,
                                 gboolean destructive);
void zg_dialogs_add_field(GtkWidget *grid,
                          gint *row,
                          const gchar *label,
                          GtkWidget *widget,
                          const gchar *hint);

/* ------------------------------------------------------------------------- */
/* 语义化操作对话框：用户只看到向导，不接触命令行参数                            */
/* ------------------------------------------------------------------------- */

typedef struct
{
    const gchar *label; /* 复选框文字 */
    const gchar *hint;  /* 说明 */
    gboolean active;    /* 默认勾选 */
} ZgChoice;

typedef void (*ZgChoiceFunc)(const gboolean *selected, gpointer data);

/* 「确认 + 选项」对话框的参数 */
typedef struct
{
    const gchar *title;
    const gchar *heading;
    const gchar *description;
    const ZgChoice *choices;
    guint n_choices;
    const gchar *accept_label;
    gboolean destructive;
} ZgChoiceSpec;

void zg_dialogs_choice(GtkWindow *parent,
                       const ZgChoiceSpec *spec,
                       ZgChoiceFunc on_accept,
                       gpointer data);

typedef void (*ZgInputFunc)(const gchar *text, gpointer data);

/* 系统文件选择器：选中后把路径写回 entry */
void zg_dialogs_pick_save(GtkWindow *parent, GtkWidget *entry,
                          const gchar *suggested_name);
void zg_dialogs_pick_open(GtkWindow *parent, GtkWidget *entry);

/* 单字段输入对话框的参数 */
typedef struct
{
    const gchar *title;
    const gchar *heading;
    const gchar *description;
    const gchar *field_label;
    const gchar *placeholder;
    const gchar *default_value;
    const gchar *accept_label;
    gboolean destructive;
} ZgInputSpec;

void zg_dialogs_input(GtkWindow *parent,
                      const ZgInputSpec *spec,
                      ZgInputFunc on_accept,
                      gpointer data);

/* 给池添加磁盘：可视化的磁盘勾选 + 用途 + 拓扑 */
void zg_dialogs_add_vdev(GtkWindow *parent,
                         const gchar *pool,
                         const gchar *const *used_devices,
                         ZgDialogDoneFunc done,
                         gpointer data);

/* 更换磁盘 */
void zg_dialogs_replace_disk(GtkWindow *parent,
                             const gchar *pool,
                             const gchar *vdev,
                             const gchar *const *used_devices,
                             ZgDialogDoneFunc done,
                             gpointer data);

/* 导入存储池：扫描并列出可导入的池 */
void zg_dialogs_import_pool(GtkWindow *parent,
                            ZgDialogDoneFunc done,
                            gpointer data);

/* 加密密钥管理 */
void zg_dialogs_encryption(GtkWindow *parent,
                           const gchar *dataset,
                           const gchar *keystatus);

/* 委派权限 */
void zg_dialogs_delegation(GtkWindow *parent,
                           const gchar *dataset);

/* 用户/组配额 */
void zg_dialogs_quota(GtkWindow *parent,
                      const gchar *dataset);

/* NFS / SMB 共享 */
void zg_dialogs_share(GtkWindow *parent,
                      const gchar *dataset);

G_END_DECLS

#endif /* ZG_DIALOGS_H */
