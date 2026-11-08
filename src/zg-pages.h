/* zg-pages.h - 各功能页面构造函数
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_PAGES_H
#define ZG_PAGES_H

#include <gtk/gtk.h>

#include "zg-app.h"

G_BEGIN_DECLS

GtkWidget *zg_page_pools_new(ZgApp *app);
GtkWidget *zg_page_datasets_new(ZgApp *app);
GtkWidget *zg_page_snapshots_new(ZgApp *app);
GtkWidget *zg_page_sendrecv_new(ZgApp *app);
GtkWidget *zg_page_be_new(ZgApp *app);
GtkWidget *zg_page_system_new(ZgApp *app);

GtkWidget *zg_page_ops_new(ZgApp *app);
GtkWidget *zg_page_help_new(ZgApp *app);

/* 帮助页字号缩放（Ctrl+加号/减号/0），返回是否处理 */
gboolean zg_page_help_zoom(gint delta);
void zg_page_ops_show(GtkWindow *parent, ZgApp *app);

G_END_DECLS

#endif /* ZG_PAGES_H */
