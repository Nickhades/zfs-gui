/* zg-page-ops.c - 命令中心（命令行等价物）
 *
 * 对象面板覆盖了日常操作；这里列出注册表中的全部受控操作，
 * 作为冷门功能的兜底入口。每条目双击打开自动生成的表单。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-pages.h"

#include <string.h>

#include "zg-app.h"
#include "zg-dialogs.h"
#include "zg-opdef.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GtkWidget *list;
    GtkWidget *search;
    GtkWidget *category;
    gchar *needle;
    gint category_index; /* 0 = 全部，其余为类别 + 1 */
} OpsPage;

/* ------------------------------------------------------------------------- */
/* 过滤                                                                       */
/* ------------------------------------------------------------------------- */

static gboolean
ops_row_filter(GtkListBoxRow *row,
               gpointer user_data)
{
    OpsPage *page = user_data;
    gint category = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "op-cat"));
    const gchar *text = g_object_get_data(G_OBJECT(row), "search-text");
    gchar *haystack;
    gchar *needle;
    gboolean matched;

    if (page->category_index > 0 && category != page->category_index - 1)
        return FALSE;

    if (page->needle == NULL || page->needle[0] == '\0')
        return TRUE;

    haystack = g_utf8_strdown(text != NULL ? text : "", -1);
    needle = g_utf8_strdown(page->needle, -1);
    matched = strstr(haystack, needle) != NULL;

    g_free(haystack);
    g_free(needle);

    return matched;
}

static void
ops_invalidate(OpsPage *page)
{
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(page->list));
}

/* ------------------------------------------------------------------------- */
/* 回调                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_search_changed(GtkSearchEntry *entry,
                  gpointer user_data)
{
    OpsPage *page = user_data;

    g_free(page->needle);
    page->needle = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    ops_invalidate(page);
}

static void
on_category_changed(GObject *object,
                    GParamSpec *pspec,
                    gpointer user_data)
{
    OpsPage *page = user_data;

    (void)pspec;

    page->category_index = (gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    ops_invalidate(page);
}

static void
on_row_activated(GtkListBox *list,
                 GtkListBoxRow *row,
                 gpointer user_data)
{
    OpsPage *page = user_data;
    const gchar *op_id = g_object_get_data(G_OBJECT(row), "op-id");
    GtkWindow *window;

    (void)list;

    if (op_id == NULL)
        return;

    window = GTK_WINDOW(gtk_widget_get_root(page->root));

    zg_dialogs_generic_op(window, op_id, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* 行构建                                                                     */
/* ------------------------------------------------------------------------- */

static GtkWidget *
build_row(const ZgOpDef *op)
{
    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = zg_ui_label(op->title != NULL ? op->title : op->id);
    GtkWidget *badge = zg_ui_label(zg_op_category_label(op->category));
    GtkWidget *description;
    GtkWidget *command;
    gchar *command_text;
    gchar *search_text;

    gtk_widget_add_css_class(title, "zg-cell-bold");
    gtk_widget_add_css_class(badge, "zg-pill");
    gtk_widget_add_css_class(badge, "zg-pill-grey");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(title_row), title);
    gtk_box_append(GTK_BOX(title_row), badge);

    if (op->auth == ZG_AUTH_PASSWORD)
    {
        GtkWidget *lock = gtk_image_new_from_icon_name("channel-secure-symbolic");

        gtk_image_set_pixel_size(GTK_IMAGE(lock), 12);
        gtk_widget_set_tooltip_text(lock, "需要管理员密码");
        gtk_box_append(GTK_BOX(title_row), lock);
    }

    description = zg_ui_label_wrap(op->description != NULL ? op->description : "");
    gtk_widget_add_css_class(description, "zg-dim");

    command_text = g_strdup_printf("%s %s", op->tool != NULL ? op->tool : "",
                                   op->subcommand != NULL ? op->subcommand : "");
    command = zg_ui_label(command_text);
    gtk_widget_add_css_class(command, "zg-mono");
    gtk_widget_add_css_class(command, "zg-dim");

    gtk_box_append(GTK_BOX(row_box), title_row);
    gtk_box_append(GTK_BOX(row_box), description);
    gtk_box_append(GTK_BOX(row_box), command);

    search_text = g_strdup_printf("%s %s %s", op->id,
                                  op->title != NULL ? op->title : "",
                                  op->description != NULL ? op->description : "");
    g_object_set_data_full(G_OBJECT(row_box), "search-text", search_text, g_free);

    g_free(command_text);

    return row_box;
}

/* ------------------------------------------------------------------------- */
/* 页面                                                                       */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_page_ops_new(ZgApp *app)
{
    OpsPage *page = g_new0(OpsPage, 1);
    GtkWidget *header;
    GtkWidget *toolbar;
    GtkWidget *scrolled;
    GtkStringList *categories;
    guint i;

    page->app = app;
    page->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(page->root, 12);
    gtk_widget_set_margin_bottom(page->root, 12);
    gtk_widget_set_margin_start(page->root, 12);
    gtk_widget_set_margin_end(page->root, 12);

    header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    {
        GtkWidget *title = zg_ui_label_heading("命令中心");
        GtkWidget *subtitle = zg_ui_label_wrap(
                "覆盖 zpool / zfs / bectl 的全部受控操作。双击条目打开表单，"
                "表单会实时显示最终命令；危险操作仍需管理员密码。");

        gtk_widget_add_css_class(subtitle, "zg-dim");
        gtk_box_append(GTK_BOX(header), title);
        gtk_box_append(GTK_BOX(header), subtitle);
    }
    gtk_box_append(GTK_BOX(page->root), header);

    categories = gtk_string_list_new(NULL);
    gtk_string_list_append(categories, "全部类别");

    for (i = 0; i < ZG_CAT_COUNT; i++)
    {
        ZgOpCategory category = (ZgOpCategory)i;

        gtk_string_list_append(categories, zg_op_category_label(category));
    }

    toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->category = gtk_drop_down_new(G_LIST_MODEL(categories), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(page->category), 0);
    g_signal_connect(page->category, "notify::selected",
                     G_CALLBACK(on_category_changed), page);

    page->search = gtk_search_entry_new();
    gtk_widget_set_hexpand(page->search, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(page->search),
                                          "搜索操作名称、ID 或说明…");
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_search_changed), page);

    gtk_box_append(GTK_BOX(toolbar), page->category);
    gtk_box_append(GTK_BOX(toolbar), page->search);
    gtk_box_append(GTK_BOX(page->root), toolbar);

    page->list = gtk_list_box_new();
    gtk_widget_add_css_class(page->list, "zg-ops-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(page->list), FALSE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(page->list), ops_row_filter, page, NULL);
    g_signal_connect(page->list, "row-activated", G_CALLBACK(on_row_activated), page);

    for (i = 0; i < zg_op_count(); i++)
    {
        const ZgOpDef *op = zg_op_at(i);
        GtkWidget *row;

        if (op == NULL || op->id == NULL)
            continue;

        row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), build_row(op));
        g_object_set_data_full(G_OBJECT(row), "op-id", g_strdup(op->id), g_free);
        g_object_set_data(G_OBJECT(row), "op-cat",
                          GINT_TO_POINTER((gint)op->category));
        gtk_list_box_append(GTK_LIST_BOX(page->list), row);
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->list);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(page->root), scrolled);

    g_object_set_data_full(G_OBJECT(page->root), "zg-ops-page", page, g_free);

    return page->root;
}

void zg_page_ops_show(GtkWindow *parent,
                      ZgApp *app)
{
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *row;
    GtkWidget *close_button;

    window = zg_dialogs_window_new(parent, "命令中心（命令行等价物）", 940, 720);
    body = zg_dialogs_body(window, NULL);
    gtk_box_append(GTK_BOX(body), zg_page_ops_new(app));

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    close_button = gtk_button_new_with_label("关闭");
    gtk_widget_add_css_class(close_button, "suggested-action");
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_close), window);
    gtk_box_append(GTK_BOX(row), close_button);
    gtk_box_append(GTK_BOX(body), row);

    gtk_window_present(window);
}
