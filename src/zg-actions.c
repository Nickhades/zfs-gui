/* zg-actions.c - 对象操作卡片
 *
 * 布局：
 *   ┌──────────────────────────────────────────────────────────┐
 *   │ [图标]  对象名                          [健康状态胶囊]     │
 *   │         副标题 / 容量说明                                  │
 *   │ ▓▓▓▓▓▓▓░░░░░░  容量进度条（可选）                          │
 *   │ [主操作] [操作] [操作] [操作] …        [更多操作 ▾]        │
 *   │ 键盘提示                                                  │
 *   └──────────────────────────────────────────────────────────┘
 *
 * 主操作最多显示 5 个，其余收进「更多操作」菜单，避免按钮墙。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-actions.h"

#include "zg-model.h"
#include "zg-ui.h"

#define ZG_ACTION_VISIBLE_MAX 5

typedef struct
{
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *state;
    GtkWidget *usage_row;
    GtkWidget *usage_bar;
    GtkWidget *usage_label;
    GtkWidget *actions_row;
    GtkWidget *flow;
    GtkWidget *hint;
} ActionPanel;

/* ------------------------------------------------------------------------- */
/* 按钮                                                                       */
/* ------------------------------------------------------------------------- */

static GtkWidget *
action_button_new(const ZgAction *action,
                  gboolean compact)
{
    GtkWidget *button;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    if (action->icon != NULL)
    {
        GtkWidget *image = gtk_image_new_from_icon_name(action->icon);

        gtk_image_set_pixel_size(GTK_IMAGE(image), 15);
        gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), image);
    }

    gtk_box_append(GTK_BOX(box), gtk_label_new(action->label));

    if (action->needs_password)
    {
        GtkWidget *lock = gtk_image_new_from_icon_name("channel-secure-symbolic");

        gtk_image_set_pixel_size(GTK_IMAGE(lock), 12);
        gtk_widget_set_valign(lock, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(lock, "需要管理员密码");
        gtk_box_append(GTK_BOX(box), lock);
    }

    if (compact)
    {
        button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), box);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    }
    else
    {
        button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), box);

        if (action->danger)
            gtk_widget_add_css_class(button, "destructive-action");
        else if (action->primary)
            gtk_widget_add_css_class(button, "suggested-action");
    }

    if (action->tooltip != NULL)
        gtk_widget_set_tooltip_text(button, action->tooltip);

    if (action->callback != NULL)
        g_signal_connect(button, "clicked", action->callback, action->data);

    return button;
}

/* ------------------------------------------------------------------------- */
/* 面板                                                                       */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_action_panel_new(void)
{
    ActionPanel *panel = g_new0(ActionPanel, 1);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *header;
    GtkWidget *texts;
    GtkWidget *actions;

    gtk_widget_add_css_class(root, "zg-object-card");

    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    panel->icon = gtk_image_new_from_icon_name("drive-harddisk-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(panel->icon), 30);
    gtk_widget_set_valign(panel->icon, GTK_ALIGN_START);
    gtk_widget_add_css_class(panel->icon, "zg-object-icon");

    texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(texts, TRUE);
    gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);

    panel->title = zg_ui_label_heading("未选择对象");
    gtk_widget_add_css_class(panel->title, "zg-object-name");
    panel->subtitle = zg_ui_label_dim("在左侧点击一个池、磁盘、数据集或快照");
    gtk_widget_add_css_class(panel->subtitle, "zg-object-sub");

    gtk_box_append(GTK_BOX(texts), panel->title);
    gtk_box_append(GTK_BOX(texts), panel->subtitle);

    panel->state = zg_ui_label("");
    gtk_widget_set_valign(panel->state, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(panel->state, "zg-pill");
    gtk_widget_set_visible(panel->state, FALSE);

    gtk_box_append(GTK_BOX(header), panel->icon);
    gtk_box_append(GTK_BOX(header), texts);
    gtk_box_append(GTK_BOX(header), panel->state);
    gtk_box_append(GTK_BOX(root), header);

    /* 容量进度条 */
    panel->usage_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    panel->usage_bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(panel->usage_bar, "zg-usage");
    gtk_widget_set_hexpand(panel->usage_bar, TRUE);
    gtk_widget_set_valign(panel->usage_bar, GTK_ALIGN_CENTER);
    panel->usage_label = zg_ui_label_dim("");
    gtk_widget_add_css_class(panel->usage_label, "zg-usage-label");

    gtk_box_append(GTK_BOX(panel->usage_row), panel->usage_bar);
    gtk_box_append(GTK_BOX(panel->usage_row), panel->usage_label);
    gtk_widget_set_visible(panel->usage_row, FALSE);
    gtk_box_append(GTK_BOX(root), panel->usage_row);

    {
        GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

        gtk_widget_add_css_class(separator, "zg-card-separator");
        gtk_box_append(GTK_BOX(root), separator);
    }

    /* 操作按钮行 */
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    panel->actions_row = actions;

    panel->flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(panel->flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(panel->flow), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(panel->flow), 6);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(panel->flow), 12);
    gtk_widget_set_hexpand(panel->flow, TRUE);

    gtk_box_append(GTK_BOX(actions), panel->flow);
    gtk_box_append(GTK_BOX(root), actions);

    panel->hint = zg_ui_label_dim("");
    gtk_widget_add_css_class(panel->hint, "zg-action-hint");
    gtk_box_append(GTK_BOX(root), panel->hint);

    g_object_set_data_full(G_OBJECT(root), "zg-action-panel", panel, g_free);

    return root;
}

static void
flow_clear(GtkWidget *flow)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(flow)) != NULL)
        gtk_flow_box_remove(GTK_FLOW_BOX(flow), child);
}

/* 右侧可能存在的「更多操作」按钮 */
static void
clear_more_button(ActionPanel *panel)
{
    GtkWidget *child = gtk_widget_get_last_child(panel->actions_row);

    if (child != NULL && child != panel->flow)
    {
        gtk_box_remove(GTK_BOX(panel->actions_row), child);
    }
}

static void
on_more_item_clicked(GtkButton *button,
                     gpointer user_data)
{
    GtkPopover *popover = user_data;

    (void)button;

    gtk_popover_popdown(popover);
}

static GtkWidget *
build_more_button(ActionPanel *panel,
                  const ZgAction *actions,
                  guint start,
                  guint count)
{
    GtkWidget *menu_button = gtk_menu_button_new();
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *child;
    guint i;

    child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    {
        GtkWidget *image = gtk_image_new_from_icon_name("view-more-symbolic");

        gtk_image_set_pixel_size(GTK_IMAGE(image), 15);
        gtk_box_append(GTK_BOX(child), image);
        gtk_box_append(GTK_BOX(child), gtk_label_new("更多操作"));
    }

    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button), child);
    gtk_widget_add_css_class(menu_button, "zg-more-button");
    gtk_widget_set_valign(menu_button, GTK_ALIGN_CENTER);

    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);

    for (i = 0; i < count; i++)
    {
        GtkWidget *item = action_button_new(&actions[start + i], TRUE);

        g_signal_connect(item, "clicked", G_CALLBACK(on_more_item_clicked), popover);
        gtk_box_append(GTK_BOX(box), item);
    }

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button), popover);

    return menu_button;
}

void zg_action_panel_set(GtkWidget *panel_widget, const ZgPanelSpec *spec)
{
    ActionPanel *panel;
    guint visible;
    guint i;

    g_return_if_fail(panel_widget != NULL);
    g_return_if_fail(spec != NULL);

    panel = g_object_get_data(G_OBJECT(panel_widget), "zg-action-panel");

    if (panel == NULL)
        return;

    gtk_image_set_from_icon_name(GTK_IMAGE(panel->icon),
                                 spec->icon != NULL ? spec->icon
                                                    : "drive-harddisk-symbolic");
    gtk_label_set_text(GTK_LABEL(panel->title),
                       spec->title != NULL ? spec->title : "未选择对象");
    gtk_label_set_text(GTK_LABEL(panel->subtitle),
                       spec->subtitle != NULL ? spec->subtitle : "");

    if (spec->state != NULL && spec->state[0] != '\0')
    {
        gtk_label_set_text(GTK_LABEL(panel->state), spec->state);
        gtk_widget_set_visible(panel->state, TRUE);
        gtk_widget_remove_css_class(panel->state, "zg-pill-green");
        gtk_widget_remove_css_class(panel->state, "zg-pill-red");
        gtk_widget_remove_css_class(panel->state, "zg-pill-grey");
        gtk_widget_add_css_class(panel->state,
                                 zg_model_state_is_healthy(spec->state)
                                         ? "zg-pill-green"
                                         : "zg-pill-red");
    }
    else
    {
        gtk_widget_set_visible(panel->state, FALSE);
    }

    /* 每次重建都先隐藏容量条，交给 set_usage 决定是否显示 */
    gtk_widget_set_visible(panel->usage_row, FALSE);

    flow_clear(panel->flow);
    clear_more_button(panel);

    visible = MIN(spec->n_actions, ZG_ACTION_VISIBLE_MAX);

    for (i = 0; i < visible; i++)
        gtk_flow_box_append(GTK_FLOW_BOX(panel->flow),
                            action_button_new(&spec->actions[i], FALSE));

    if (spec->n_actions > visible)
    {
        GtkWidget *more = build_more_button(panel, spec->actions, visible,
                                            spec->n_actions - visible);

        gtk_box_append(GTK_BOX(panel->actions_row), more);
    }

    gtk_label_set_text(GTK_LABEL(panel->hint),
                       spec->hint != NULL ? spec->hint : "");
    gtk_widget_set_visible(panel->hint,
                           spec->hint != NULL && spec->hint[0] != '\0');
}

void zg_action_panel_set_usage(GtkWidget *panel_widget,
                               gdouble fraction,
                               const gchar *label)
{
    ActionPanel *panel;

    g_return_if_fail(panel_widget != NULL);

    panel = g_object_get_data(G_OBJECT(panel_widget), "zg-action-panel");

    if (panel == NULL)
        return;

    if (fraction < 0.0)
    {
        gtk_widget_set_visible(panel->usage_row, FALSE);
        return;
    }

    if (fraction > 1.0)
        fraction = 1.0;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(panel->usage_bar), fraction);
    gtk_label_set_text(GTK_LABEL(panel->usage_label), label != NULL ? label : "");
    gtk_widget_set_visible(panel->usage_row, TRUE);

    if (fraction >= 0.9)
        gtk_widget_add_css_class(panel->usage_bar, "zg-usage-critical");
    else if (fraction >= 0.75)
        gtk_widget_add_css_class(panel->usage_bar, "zg-usage-warn");
    else
    {
        gtk_widget_remove_css_class(panel->usage_bar, "zg-usage-warn");
        gtk_widget_remove_css_class(panel->usage_bar, "zg-usage-critical");
    }
}
