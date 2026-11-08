/* zg-table.c - 基于 GtkColumnView 的表格
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-table.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* ZgRow                                                                      */
/* ------------------------------------------------------------------------- */

#define ZG_ROW_MAX_COLUMNS 10

struct _ZgRow
{
    GObject parent_instance;

    gchar *text[ZG_ROW_MAX_COLUMNS];
    GListStore *children;
};

G_DEFINE_FINAL_TYPE(ZgRow, zg_row, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_C0,
    PROP_C1,
    PROP_C2,
    PROP_C3,
    PROP_C4,
    PROP_C5,
    PROP_C6,
    PROP_C7,
    PROP_C8,
    PROP_C9,
    N_PROPS
};

static GParamSpec *row_properties[N_PROPS];
static const gchar *const row_property_names[ZG_ROW_MAX_COLUMNS] = {
        "c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"};

static void
zg_row_finalize(GObject *object)
{
    ZgRow *self = ZG_ROW(object);
    guint i;

    for (i = 0; i < ZG_ROW_MAX_COLUMNS; i++)
        g_free(self->text[i]);

    g_clear_object(&self->children);

    G_OBJECT_CLASS(zg_row_parent_class)->finalize(object);
}

static void
zg_row_get_property(GObject *object,
                    guint property_id,
                    GValue *value,
                    GParamSpec *pspec)
{
    ZgRow *self = ZG_ROW(object);
    guint index;

    if (property_id <= PROP_0 || property_id >= N_PROPS)
    {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        return;
    }

    index = property_id - PROP_C0;
    g_value_set_string(value, self->text[index] != NULL ? self->text[index] : "");
}

static void
zg_row_set_property(GObject *object,
                    guint property_id,
                    const GValue *value,
                    GParamSpec *pspec)
{
    ZgRow *self = ZG_ROW(object);
    guint index;

    if (property_id <= PROP_0 || property_id >= N_PROPS)
    {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        return;
    }

    index = property_id - PROP_C0;
    g_free(self->text[index]);
    self->text[index] = g_value_dup_string(value);
}

static void
zg_row_class_init(ZgRowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    guint i;

    object_class->finalize = zg_row_finalize;
    object_class->get_property = zg_row_get_property;
    object_class->set_property = zg_row_set_property;

    for (i = 0; i < ZG_ROW_MAX_COLUMNS; i++)
    {
        row_properties[PROP_C0 + i] =
                g_param_spec_string(row_property_names[i], NULL, NULL, "",
                                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    }

    g_object_class_install_properties(object_class, N_PROPS, row_properties);
}

static void
zg_row_init(ZgRow *self)
{
    (void)self;
}

ZgRow *
zg_row_new(void)
{
    return g_object_new(ZG_TYPE_ROW, NULL);
}

void zg_row_set_text(ZgRow *row,
                     guint index,
                     const gchar *text)
{
    g_return_if_fail(ZG_IS_ROW(row));
    g_return_if_fail(index < ZG_ROW_MAX_COLUMNS);

    g_object_set(row, row_property_names[index], text != NULL ? text : "", NULL);
}

const gchar *
zg_row_get_text(ZgRow *row,
                guint index)
{
    g_return_val_if_fail(ZG_IS_ROW(row), NULL);

    if (index >= ZG_ROW_MAX_COLUMNS)
        return NULL;

    return row->text[index];
}

void zg_row_add_child(ZgRow *row,
                      ZgRow *child)
{
    g_return_if_fail(ZG_IS_ROW(row));
    g_return_if_fail(ZG_IS_ROW(child));

    if (row->children == NULL)
        row->children = g_list_store_new(ZG_TYPE_ROW);

    g_list_store_append(row->children, child);
}

GListStore *
zg_row_get_children(ZgRow *row)
{
    g_return_val_if_fail(ZG_IS_ROW(row), NULL);

    return row->children;
}

void zg_row_set_tag(ZgRow *row,
                    gpointer tag,
                    GDestroyNotify destroy)
{
    g_return_if_fail(ZG_IS_ROW(row));

    g_object_set_data_full(G_OBJECT(row), "zg-row-tag", tag, destroy);
}

gpointer
zg_row_get_tag(ZgRow *row)
{
    g_return_val_if_fail(ZG_IS_ROW(row), NULL);

    return g_object_get_data(G_OBJECT(row), "zg-row-tag");
}

/* ------------------------------------------------------------------------- */
/* 列工厂                                                                     */
/* ------------------------------------------------------------------------- */

static void
column_setup(GtkSignalListItemFactory *factory,
             GtkListItem *item,
             gpointer user_data)
{
    const ZgTableColumn *column = user_data;
    GtkWidget *label = gtk_label_new(NULL);

    (void)factory;

    gtk_label_set_xalign(GTK_LABEL(label), column->xalign);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    if (column->min_width > 0)
        gtk_label_set_width_chars(GTK_LABEL(label), column->min_width);

    gtk_widget_set_margin_top(label, 5);
    gtk_widget_set_margin_bottom(label, 5);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);

    if (column->bold)
        gtk_widget_add_css_class(label, "zg-cell-bold");
    if (column->dim)
        gtk_widget_add_css_class(label, "zg-cell-dim");

    if (column->index == 0)
    {
        GtkWidget *expander = gtk_tree_expander_new();

        gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), label);
        gtk_list_item_set_child(item, expander);
    }
    else
    {
        gtk_list_item_set_child(item, label);
    }
}

static void
column_unbind(GtkSignalListItemFactory *factory,
              GtkListItem *item,
              gpointer user_data)
{
    const ZgTableColumn *column = user_data;
    GtkWidget *child = gtk_list_item_get_child(item);
    GtkWidget *label = child;

    (void)factory;

    if (column->index == 0)
        label = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(child));

    if (label != NULL)
        g_object_set_data(G_OBJECT(label), "zg-binding", NULL);
}

static void
column_bind(GtkSignalListItemFactory *factory,
            GtkListItem *item,
            gpointer user_data)
{
    const ZgTableColumn *column = user_data;
    GtkWidget *child = gtk_list_item_get_child(item);
    GtkWidget *label = child;
    GObject *object = gtk_list_item_get_item(item);
    GtkTreeListRow *list_row = NULL;

    (void)factory;

    if (GTK_IS_TREE_LIST_ROW(object))
        list_row = GTK_TREE_LIST_ROW(object);

    if (column->index == 0)
    {
        gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(child), list_row);
        label = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(child));
        object = list_row != NULL ? gtk_tree_list_row_get_item(list_row) : object;
    }
    else if (list_row != NULL)
    {
        object = gtk_tree_list_row_get_item(list_row);
    }

    /* 用属性绑定代替一次性 set_text：页面只改 ZgRow 的文本，
     * 表格会随 GObject 通知自动刷新（容量等实时数据用得上）。 */
    {
        GBinding *binding = g_object_bind_property(object,
                                                   row_property_names[column->index],
                                                   label, "label",
                                                   G_BINDING_SYNC_CREATE);
        g_object_set_data_full(G_OBJECT(label), "zg-binding", binding, g_object_unref);
    }
}

/* ------------------------------------------------------------------------- */
/* 表格                                                                       */
/* ------------------------------------------------------------------------- */

static GtkWidget *
table_common(GListModel *model,
             const ZgTableColumn *cols,
             guint n_cols)
{
    GtkWidget *view;
    GtkSelectionModel *selection;
    guint i;

    selection = GTK_SELECTION_MODEL(
            gtk_single_selection_new(G_LIST_MODEL(g_object_ref(model))));

    view = gtk_column_view_new(selection);

    gtk_widget_add_css_class(view, "zg-table");
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view), TRUE);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view), FALSE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view), FALSE);

    for (i = 0; i < n_cols; i++)
    {
        const ZgTableColumn *column = &cols[i];
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        GtkColumnViewColumn *view_column;
        ZgTableColumn *copy = g_new0(ZgTableColumn, 1);

        *copy = *column;
        g_object_set_data_full(G_OBJECT(factory), "zg-column", copy, g_free);

        g_signal_connect(factory, "setup", G_CALLBACK(column_setup), copy);
        g_signal_connect(factory, "bind", G_CALLBACK(column_bind), copy);
        g_signal_connect(factory, "unbind", G_CALLBACK(column_unbind), copy);

        view_column = gtk_column_view_column_new(column->title, factory);
        gtk_column_view_column_set_expand(view_column, column->expand);
        gtk_column_view_column_set_resizable(view_column, TRUE);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view), view_column);
    }

    return view;
}

GtkWidget *
zg_table_new_flat(GListStore *store,
                  const ZgTableColumn *cols,
                  guint n_cols)
{
    return table_common(G_LIST_MODEL(store), cols, n_cols);
}

static GListModel *
create_children(gpointer item,
                gpointer user_data)
{
    ZgRow *row = ZG_ROW(item);
    GListStore *children;

    (void)user_data;

    children = zg_row_get_children(row);

    return children != NULL ? G_LIST_MODEL(g_object_ref(children)) : NULL;
}

GtkWidget *
zg_table_new_tree(GListStore *store,
                  const ZgTableColumn *cols,
                  guint n_cols)
{
    GtkTreeListModel *tree_model;
    GtkWidget *view;

    tree_model = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(store)),
                                         FALSE, FALSE, create_children,
                                         NULL, NULL);
    gtk_tree_list_model_set_autoexpand(tree_model, TRUE);

    view = table_common(G_LIST_MODEL(tree_model), cols, n_cols);

    g_object_unref(tree_model);

    return view;
}

GtkSelectionModel *
zg_table_get_selection(GtkWidget *table)
{
    g_return_val_if_fail(GTK_IS_COLUMN_VIEW(table), NULL);

    return gtk_column_view_get_model(GTK_COLUMN_VIEW(table));
}

ZgRow *
zg_table_get_selected(GtkWidget *table)
{
    GtkSelectionModel *selection = zg_table_get_selection(table);
    GObject *item;

    if (selection == NULL || !GTK_IS_SINGLE_SELECTION(selection))
        return NULL;

    item = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(selection));

    if (item != NULL && GTK_IS_TREE_LIST_ROW(item))
        item = gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(item));

    return (item != NULL && ZG_IS_ROW(item)) ? ZG_ROW(item) : NULL;
}

static gboolean
select_first_idle(gpointer data)
{
    GtkWidget *table = data;
    GtkSelectionModel *selection = zg_table_get_selection(table);

    if (selection != NULL && g_list_model_get_n_items(G_LIST_MODEL(selection)) > 0)
        gtk_selection_model_select_item(selection, 0, TRUE);

    return G_SOURCE_REMOVE;
}

void zg_table_select_first(GtkWidget *table)
{
    GtkSelectionModel *selection = zg_table_get_selection(table);
    guint n_items;

    if (selection == NULL)
        return;

    n_items = g_list_model_get_n_items(G_LIST_MODEL(selection));

    if (n_items > 0)
    {
        /* 已经是第一项时 select_item 不会发信号，手动补一次，
         * 确保进入页面/刷新后对象面板立即显示。 */
        if (gtk_selection_model_is_selected(selection, 0))
            g_signal_emit_by_name(selection, "selection-changed", 0u, n_items);
        else
            gtk_selection_model_select_item(selection, 0, TRUE);
        return;
    }

    /* 树模型的行是在主循环里生成的：此刻还没行时，下一轮空闲再选 */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, select_first_idle,
                    g_object_ref(table), g_object_unref);
}
