/* zg-table.h - 基于 GtkColumnView 的表格
 *
 * 与 GtkTreeView 相比：
 *   · 每一行是真实控件，可以画行间横线（网格感）、悬停与斑马纹；
 *   · 支持层级（GtkTreeListModel），用于存储池 vdev 树与数据集树；
 *   · 列表项携带自定义 tag，页面用它保存业务数据。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define ZG_TYPE_ROW (zg_row_get_type())
G_DECLARE_FINAL_TYPE(ZgRow, zg_row, ZG, ROW, GObject)

/* 行对象：最多 8 个文本列 + 一个业务 tag + 可选子行 */
ZgRow *zg_row_new(void);
void zg_row_set_text(ZgRow *row,
                     guint index,
                     const gchar *text);
const gchar *zg_row_get_text(ZgRow *row,
                             guint index);
void zg_row_add_child(ZgRow *row,
                      ZgRow *child);
GListStore *zg_row_get_children(ZgRow *row);

void zg_row_set_tag(ZgRow *row,
                    gpointer tag,
                    GDestroyNotify destroy);
gpointer zg_row_get_tag(ZgRow *row);

/* 列定义 */
typedef struct
{
    const gchar *title;
    guint index;     /* zg_row 的列号，0..7 */
    gboolean expand; /* 占据剩余宽度 */
    gint min_width;  /* 字符宽度，0 表示默认 */
    gfloat xalign;   /* 0 左对齐，1 右对齐 */
    gboolean bold;
    gboolean dim;
} ZgTableColumn;

/* 用现有 store 创建表格；store 由页面持有并在刷新时清空/重建 */
GtkWidget *zg_table_new_flat(GListStore *store,
                             const ZgTableColumn *cols,
                             guint n_cols);
GtkWidget *zg_table_new_tree(GListStore *store,
                             const ZgTableColumn *cols,
                             guint n_cols);

GtkSelectionModel *zg_table_get_selection(GtkWidget *table);
ZgRow *zg_table_get_selected(GtkWidget *table); /* transfer none */
void zg_table_select_first(GtkWidget *table);

G_END_DECLS
