/* zg-ui.h - 共享界面组件与样式
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_UI_H
#define ZG_UI_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* 安装全局 CSS，只需在启动时调用一次 */
void zg_ui_init(void);

/* 常用容器 */
GtkWidget *zg_ui_toolbar_new(void);
void zg_ui_toolbar_add(GtkWidget *toolbar,
                       GtkWidget *widget);
GtkWidget *zg_ui_button(const gchar *label,
                        GCallback callback,
                        gpointer data);
GtkWidget *zg_ui_check(const gchar *label);

GtkWidget *zg_ui_label(const gchar *text);
GtkWidget *zg_ui_label_dim(const gchar *text);
GtkWidget *zg_ui_label_heading(const gchar *text);
GtkWidget *zg_ui_label_mono(const gchar *text);
GtkWidget *zg_ui_label_wrap(const gchar *text);

/* 属性网格：返回创建出来的值标签，便于后续更新 */
GtkWidget *zg_ui_grid_new(void);
GtkWidget *zg_ui_grid_add(GtkWidget *grid,
                          gint *row,
                          const gchar *label,
                          const gchar *value);
GtkWidget *zg_ui_grid_add_widget(GtkWidget *grid,
                                 gint *row,
                                 const gchar *label,
                                 GtkWidget *widget);

/* 只读文本视图 */
GtkWidget *zg_ui_text_view_new(GtkTextBuffer **buffer_out,
                               gboolean wrap);
void zg_ui_text_set(GtkTextBuffer *buffer,
                    const gchar *text);

/* 树视图 + 文本列 */
GtkWidget *zg_ui_tree_view_new(GtkTreeModel *model);
GtkTreeViewColumn *zg_ui_add_column(GtkTreeView *view,
                                    const gchar *title,
                                    gint column_id,
                                    gboolean expand);

/* 带颜色的状态标签 */
GtkWidget *zg_ui_state_label_new(const gchar *state);
void zg_ui_label_set_text(GtkWidget *label,
                          const gchar *text);
void zg_ui_state_label_set(GtkWidget *label,
                           const gchar *state);

/* 简易提示条（用于连接失败等） */
GtkWidget *zg_ui_banner_new(void);
void zg_ui_banner_set(GtkWidget *banner,
                      const gchar *title,
                      const gchar *detail,
                      gboolean warning);

G_END_DECLS

#endif /* ZG_UI_H */
