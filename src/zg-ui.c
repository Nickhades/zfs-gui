/* zg-ui.c - 共享界面组件与样式实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-ui.h"

#include <string.h>

#include "zg-model.h"

static const gchar zg_css[] =
        "@define-color zg_bg #f4f6f3;"
        "\n"
        "* { font-weight: 700; }"
        "\n"

        "@define-color zg_surface #ffffff;"
        "\n"
        "@define-color zg_surface_alt #edf1ec;"
        "\n"
        "@define-color zg_border rgba(24, 40, 30, 0.12);"
        "\n"
        "@define-color zg_text #1c2a21;"
        "\n"
        "@define-color zg_text_dim #5f7064;"
        "\n"
        "@define-color zg_accent #3fb883;"
        "\n"
        "@define-color zg_accent_hover #34a572;"
        "\n"
        "@define-color zg_accent_soft #d9f2e6;"
        "\n"
        "@define-color zg_danger #d9534f;"
        "\n"
        "@define-color zg_warn #e0a800;"
        "\n"
        ""
        "\n"
        "window { background-color: @zg_bg; color: @zg_text; font-family: \"Noto Sans CJK SC\", \"Source Han Sans SC\", \"Noto Sans\", sans-serif; font-weight: 700; }"
        "\n"
        "headerbar { background-color: @zg_surface; color: @zg_text; border-bottom: 1px solid @zg_border; min-height: 46px; }"
        "\n"
        ""
        "\n"
        ".zg-page-title { font-size: 1.18em; font-weight: 800; color: @zg_text; }"
        "\n"
        ".zg-page-subtitle { font-size: 0.80em; font-weight: 700; color: @zg_text_dim; opacity: 0.85; }"
        "\n"
        ""
        "\n"
        ".zg-sidebar { background-color: @zg_surface_alt; border-right: 1px solid @zg_border; }"
        "\n"
        ".zg-brand-title { font-weight: 800; font-size: 1.06em; color: @zg_text; }"
        "\n"
        ".zg-brand-sub { font-size: 0.74em; font-weight: 700; color: @zg_text_dim; opacity: 0.85; }"
        "\n"
        ".zg-brand-tile { background-color: @zg_accent; color: #ffffff; border-radius: 9px; padding: 7px; }"
        "\n"
        ".zg-nav-title { font-weight: 700; font-size: 0.94em; color: @zg_text; }"
        "\n"
        ".zg-nav-subtitle { font-size: 0.71em; font-weight: 700; color: @zg_text_dim; opacity: 0.85; }"
        "\n"
        ".zg-sidebar-row { border-radius: 9px; margin: 2px 8px; }"
        "\n"
        ".zg-sidebar-row:hover { background-color: alpha(@zg_text, 0.06); }"
        "\n"
        ".zg-sidebar-row:selected { background-color: @zg_accent; color: #ffffff; }"
        "\n"
        ".zg-sidebar-row:selected label { color: #ffffff; }"
        "\n"
        ".zg-sidebar-row:selected .zg-nav-subtitle { color: rgba(255, 255, 255, 0.85); opacity: 1; }"
        "\n"
        ""
        "\n"
        ".zg-secure-badge { background-color: @zg_surface; border: 1px solid @zg_border; border-radius: 10px; padding: 8px 10px; }"
        "\n"
        ".zg-insecure-badge { background-color: #fdecea; border: 1px solid #f3c2bd; border-radius: 10px; padding: 8px 10px; }"
        "\n"
        ".zg-secure-title { font-size: 0.86em; font-weight: 700; color: @zg_text; }"
        "\n"
        ".zg-secure-detail { font-size: 0.74em; font-weight: 700; color: @zg_text_dim; opacity: 0.9; }"
        "\n"
        ""
        "\n"
        ".zg-object-card { background-color: @zg_surface; border: 1px solid @zg_border; border-radius: 12px; padding: 14px 16px; margin: 12px 12px 8px 12px; box-shadow: 0 1px 2px alpha(#000000, 0.04); }"
        "\n"
        ".zg-object-name { font-size: 1.30em; font-weight: 800; color: @zg_text; }"
        "\n"
        ".zg-object-sub { font-size: 0.84em; font-weight: 700; color: @zg_text_dim; }"
        "\n"
        ".zg-object-icon { color: @zg_accent; }"
        "\n"
        ".zg-object-card button { padding: 5px 11px; border-radius: 8px; }"
        "\n"
        ".zg-action-hint { font-size: 0.77em; font-weight: 700; color: @zg_text_dim; opacity: 0.8; }"
        "\n"
        ".zg-card-separator { background-color: @zg_border; min-height: 1px; margin: 2px 0; }"
        "\n"
        ""
        "\n"
        ".zg-usage { min-height: 8px; }"
        "\n"
        ".zg-usage trough { background-color: alpha(@zg_text, 0.09); border-radius: 999px; min-height: 8px; }"
        "\n"
        ".zg-usage progress { background-color: @zg_accent; border-radius: 999px; min-height: 8px; }"
        "\n"
        ".zg-usage-warn progress { background-color: @zg_warn; }"
        "\n"
        ".zg-usage-critical progress { background-color: @zg_danger; }"
        "\n"
        ".zg-usage-label { font-size: 0.77em; font-weight: 700; color: @zg_text_dim; }"
        "\n"
        ""
        "\n"
        ".zg-heading { font-weight: 800; font-size: 1.08em; color: @zg_text; }"
        "\n"
        ".zg-dim { font-weight: 700; color: @zg_text_dim; opacity: 0.9; }"
        "\n"
        ".zg-mono { font-family: \"Noto Sans Mono\", monospace; font-weight: 700; }"
        "\n"
        ".zg-danger { color: @zg_danger; font-weight: 700; }"
        "\n";

static const gchar zg_css_tail[] =

        "@define-color zg_bg #f4f6f3;"
        "\n"
        "@define-color zg_surface #ffffff;"
        "\n"
        "@define-color zg_surface_alt #edf1ec;"
        "\n"
        "@define-color zg_border rgba(24, 40, 30, 0.12);"
        "\n"
        "@define-color zg_text #1c2a21;"
        "\n"
        "@define-color zg_text_dim #5f7064;"
        "\n"
        "@define-color zg_accent #3fb883;"
        "\n"
        "@define-color zg_accent_hover #34a572;"
        "\n"
        "@define-color zg_accent_soft #d9f2e6;"
        "\n"
        "@define-color zg_danger #d9534f;"
        "\n"
        "@define-color zg_warn #e0a800;"
        "\n"
        ".zg-lock-icon { color: @zg_warn; }"
        "\n"
        ""
        "\n"
        ".zg-card { background-color: @zg_surface; border: 1px solid @zg_border; border-radius: 10px; padding: 12px; }"
        "\n"
        ".zg-pill { border-radius: 999px; padding: 2px 10px; font-size: 0.76em; font-weight: 700; }"
        "\n"
        ".zg-pill-green { background-color: @zg_accent_soft; color: #1f7a52; }"
        "\n"
        ".zg-pill-red { background-color: #fdecea; color: #b3261e; }"
        "\n"
        ".zg-pill-grey { background-color: alpha(@zg_text, 0.08); color: @zg_text_dim; }"
        "\n"
        ""
        "\n"
        ".zg-state-online { color: #2a9d6f; font-weight: 700; }"
        "\n"
        ".zg-state-degraded { color: #b07d00; font-weight: 700; }"
        "\n"
        ".zg-state-faulted { color: @zg_danger; font-weight: 700; }"
        "\n"
        ".zg-state-offline { color: #8a8a84; font-weight: 700; }"
        "\n"
        ".zg-state-unknown { color: #8a8a84; font-weight: 700; }"
        "\n"
        ""
        "\n"
        "button { border-radius: 8px; }"
        "\n"
        "button.suggested-action { background-image: none; background-color: @zg_accent; color: #ffffff; border: none; box-shadow: none; }"
        "\n"
        "button.suggested-action:hover, button.suggested-action:focus { background-image: none; background-color: @zg_accent_hover; }"
        "\n"
        "button.suggested-action:active { background-image: none; background-color: #2e9567; }"
        "\n"
        "button.destructive-action { background-image: none; background-color: #fdecea; color: #b3261e; border: 1px solid #f3c2bd; }"
        "\n"
        "button.destructive-action:hover { background-color: #fbdcd8; }"
        "\n"
        "entry { background-color: @zg_surface; border: 1px solid @zg_border; border-radius: 8px; color: @zg_text; }"
        "\n"
        "entry:focus { border-color: @zg_accent; box-shadow: 0 0 0 2px alpha(@zg_accent, 0.25); }"
        "\n"
        "checkbutton check:checked, checkbutton check:indeterminate { background-color: @zg_accent; border-color: @zg_accent; color: #ffffff; }"
        "\n"
        "switch:checked { background-color: @zg_accent; }"
        "\n"
        "progressbar progress { background-color: @zg_accent; }"
        "\n"
        "scrollbar slider { background-color: alpha(@zg_text, 0.22); border-radius: 999px; }"
        "\n"
        "selection { background-color: @zg_accent_soft; color: @zg_text; }"
        "\n"
        ""
        "\n"
        ".zg-toolbar { padding: 6px 10px; background-color: transparent; border-bottom: 1px solid @zg_border; }"
        "\n"
        ".zg-toolbar button { border-radius: 8px; padding: 4px 10px; }"
        "\n"
        ""
        "\n"
        ".zg-table row { border-bottom: 1px solid alpha(@zg_text, 0.10); }"
        "\n"
        ".zg-table row:nth-child(even) { background-color: alpha(@zg_text, 0.028); }"
        "\n"
        ".zg-table row:hover { background-color: alpha(@zg_text, 0.055); }"
        "\n"
        ".zg-table row:selected { background-color: @zg_accent_soft; color: @zg_text; }"
        "\n"

        ".zg-table header { background-color: alpha(@zg_text, 0.035); border-bottom: 1px solid @zg_border; }"
        "\n"
        ".zg-table header button { font-weight: 700; color: @zg_text_dim; }"
        "\n"
        ".zg-cell-bold { font-weight: 700; color: @zg_text; }"
        "\n"
        ".zg-cell-dim { color: @zg_text_dim; opacity: 0.85; }"
        "\n"
        ""
        "\n"
        "notebook > header { background-color: transparent; border-bottom: 1px solid @zg_border; }"
        "\n"
        "notebook > header tabs > tab { padding: 6px 12px; font-weight: 700; color: @zg_text_dim; }"
        "\n"
        "notebook > header tabs > tab:checked { color: @zg_text; box-shadow: inset 0 -2px @zg_accent; }"
        "\n"
        ""
        "\n"
        "paned > separator { background-color: @zg_border; min-width: 1px; min-height: 1px; }"
        "\n"
        ""
        "\n"
        ".zg-statusbar { background-color: @zg_surface; border-top: 1px solid @zg_border; padding: 4px 12px; }"
        "\n"
        ".zg-statusbar label { font-size: 0.80em; font-weight: 700; color: @zg_text_dim; opacity: 1; }"
        "\n"
        ""
        "\n"
        ".zg-window-control { border-radius: 999px; min-width: 26px; min-height: 26px; padding: 2px; color: @zg_text; }"
        "\n"
        ".zg-window-close:hover { background-color: @zg_danger; color: #ffffff; }"
        "\n"
        ""
        "\n"
        ".zg-toast { background-color: #1c2a21; color: #ffffff; border: 1px solid alpha(#ffffff, 0.15); border-radius: 10px; padding: 9px 18px; box-shadow: 0 6px 20px alpha(#000000, 0.25); }"
        "\n"
        ""
        "\n"
        ".zg-warning-banner { background-color: #fdf4d6; border-left: 3px solid @zg_warn; border-bottom: 1px solid #f0e2b4; padding: 5px 12px; color: #775900; }"
        "\n";

/* 第三段：命令中心列表的选中样式（避免与主题蓝色选中冲突） */
static const gchar zg_css_extra[] =
        "@define-color zg_bg #f4f6f3;"
        "\n"
        "@define-color zg_surface #ffffff;"
        "\n"
        "@define-color zg_surface_alt #edf1ec;"
        "\n"
        "@define-color zg_border rgba(24, 40, 30, 0.12);"
        "\n"
        "@define-color zg_text #1c2a21;"
        "\n"
        "@define-color zg_text_dim #5f7064;"
        "\n"
        "@define-color zg_accent #3fb883;"
        "\n"
        "@define-color zg_accent_hover #34a572;"
        "\n"
        "@define-color zg_accent_soft #d9f2e6;"
        "\n"
        "@define-color zg_danger #d9534f;"
        "\n"
        "@define-color zg_warn #e0a800;"
        "\n"
        ".zg-help-text text { font-size: 15px; }"
        "\n"
        ".zg-help-text { font-size: 15px; }"
        "\n"
        ".zg-help-list row { padding: 3px 2px; }"
        "\n"
        ".zg-help-list row label { font-size: 14px; }"
        "\n"
        ".zg-help-list .zg-dim { font-size: 13px; }"
        "\n"
        "popover > contents { background-color: @zg_surface; box-shadow: none; }"
        "\n"
        "popover { background: transparent; }"
        "\n"
        ".popover menu, popover contents { padding: 4px; }"
        "\n"
        "popover contents button { padding: 8px 14px; min-height: 26px; }"
        "\n"
        "popover contents button label { padding: 1px 0; }"
        "\n"
        ".zg-ops-list row:hover { background-color: alpha(@zg_text, 0.05); }"
        "\n"
        ".zg-ops-list row:selected { background-color: @zg_accent_soft; }"
        "\n"
        ".zg-ops-list row:selected label, .zg-ops-list row:selected .zg-dim, "
        ".zg-ops-list row:selected .zg-cell-bold, .zg-ops-list row:selected .zg-mono "
        "{ color: @zg_text; }"
        "\n";

static const gchar *const zg_state_classes[] = {
        "zg-state-online",
        "zg-state-degraded",
        "zg-state-faulted",
        "zg-state-offline",
        "zg-state-unknown",
};

/* 固定浅色主题：绿色强调色 + 白色卡片 */
static void
apply_desktop_theme(void)
{
    GtkSettings *settings = gtk_settings_get_default();

    if (settings == NULL)
        return;

    /* 允许用户用 GTK_THEME 覆盖；否则固定浅色主题，保证配色一致 */
    if (g_getenv("GTK_THEME") != NULL)
        return;

    g_object_set(settings,
                 "gtk-theme-name", "Adwaita",
                 "gtk-application-prefer-dark-theme", FALSE,
                 NULL);
}

void zg_ui_init(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();

    apply_desktop_theme();

    gtk_css_provider_load_from_data(provider, zg_css, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    {
        GtkCssProvider *tail = gtk_css_provider_new();

        gtk_css_provider_load_from_data(tail, zg_css_tail, -1);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(tail),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(tail);
    }

    {
        GtkCssProvider *extra = gtk_css_provider_new();

        gtk_css_provider_load_from_data(extra, zg_css_extra, -1);
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(extra),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(extra);
    }
}

/* ------------------------------------------------------------------------- */
/* 容器                                                                       */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_toolbar_new(void)
{
    GtkWidget *box = gtk_flow_box_new();

    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 6);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 32);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(box), 1);
    gtk_widget_add_css_class(box, "zg-toolbar");

    return box;
}

void zg_ui_toolbar_add(GtkWidget *toolbar,
                       GtkWidget *widget)
{
    gtk_flow_box_append(GTK_FLOW_BOX(toolbar), widget);
}

/* 依据按钮文字自动挑选 Adwaita 符号图标，保持全局一致的视觉语言 */
static const struct
{
    const gchar *keyword;
    const gchar *icon;
} zg_icon_map[] = {
        {"创建快照", "camera-photo-symbolic"},
        {"创建", "list-add-symbolic"},
        {"新建", "list-add-symbolic"},
        {"导入", "document-open-symbolic"},
        {"导出", "document-save-symbolic"},
        {"销毁", "edit-delete-symbolic"},
        {"删除", "edit-delete-symbolic"},
        {"刷新", "view-refresh-symbolic"},
        {"清空", "edit-clear-symbolic"},
        {"清除", "edit-clear-symbolic"},
        {"scrub", "security-high-symbolic"},
        {"TRIM", "user-trash-symbolic"},
        {"挂载", "media-playback-start-symbolic"},
        {"卸载", "media-playback-stop-symbolic"},
        {"密钥", "channel-secure-symbolic"},
        {"加密", "channel-secure-symbolic"},
        {"委派", "system-users-symbolic"},
        {"回滚", "edit-undo-symbolic"},
        {"克隆", "edit-copy-symbolic"},
        {"发送", "document-send-symbolic"},
        {"接收", "document-save-symbolic"},
        {"差异", "edit-find-symbolic"},
        {"hold", "bookmark-new-symbolic"},
        {"release", "bookmark-new-symbolic"},
        {"书签", "bookmark-new-symbolic"},
        {"激活", "emblem-ok-symbolic"},
        {"重命名", "document-edit-symbolic"},
        {"全部操作", "view-more-symbolic"},
        {"更多", "view-more-symbolic"},
        {"编辑属性", "document-edit-symbolic"},
        {"开始", "media-playback-start-symbolic"},
        {"取消", "process-stop-symbolic"},
        {"关闭", "window-close-symbolic"},
        {"重新连接", "network-transmit-receive-symbolic"},
        {"关于", "help-about-symbolic"},
        {"提示", "dialog-information-symbolic"},
        {"验证", "channel-secure-symbolic"},
        {"快照", "camera-photo-symbolic"},
};

static const gchar *
icon_for_label(const gchar *label)
{
    guint i;

    if (label == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(zg_icon_map); i++)
    {
        if (strstr(label, zg_icon_map[i].keyword) != NULL)
            return zg_icon_map[i].icon;
    }

    return NULL;
}

GtkWidget *
zg_ui_button(const gchar *label,
             GCallback callback,
             gpointer data)
{
    const gchar *icon = icon_for_label(label);
    GtkWidget *button;

    if (icon == NULL)
    {
        button = gtk_button_new_with_label(label);
    }
    else
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *image = gtk_image_new_from_icon_name(icon);

        gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), image);
        gtk_box_append(GTK_BOX(box), gtk_label_new(label));
        button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), box);
    }

    if (callback != NULL)
        g_signal_connect(button, "clicked", callback, data);

    return button;
}

GtkWidget *
zg_ui_check(const gchar *label)
{
    return gtk_check_button_new_with_label(label);
}

/* ------------------------------------------------------------------------- */
/* 标签                                                                       */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text != NULL ? text : "");

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    return label;
}

GtkWidget *
zg_ui_label_dim(const gchar *text)
{
    GtkWidget *label = zg_ui_label(text != NULL ? text : "");

    gtk_widget_add_css_class(label, "zg-dim");

    return label;
}

GtkWidget *
zg_ui_label_heading(const gchar *text)
{
    GtkWidget *label = zg_ui_label(text != NULL ? text : "");

    gtk_widget_add_css_class(label, "zg-heading");

    return label;
}

GtkWidget *
zg_ui_label_mono(const gchar *text)
{
    GtkWidget *label = zg_ui_label(text != NULL ? text : "");

    gtk_widget_add_css_class(label, "zg-mono");
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);

    return label;
}

GtkWidget *
zg_ui_label_wrap(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text != NULL ? text : "");

    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);

    return label;
}

/* ------------------------------------------------------------------------- */
/* 网格                                                                       */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_grid_new(void)
{
    GtkWidget *grid = gtk_grid_new();

    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);

    return grid;
}

GtkWidget *
zg_ui_grid_add(GtkWidget *grid,
               gint *row,
               const gchar *label,
               const gchar *value)
{
    GtkWidget *name = zg_ui_label_dim(label);
    GtkWidget *val = zg_ui_label(value != NULL ? value : "—");

    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_focusable(val, FALSE);
    gtk_grid_attach(GTK_GRID(grid), name, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), val, 1, *row, 1, 1);
    gtk_widget_set_hexpand(val, TRUE);

    (*row)++;

    return val;
}

GtkWidget *
zg_ui_grid_add_widget(GtkWidget *grid,
                      gint *row,
                      const gchar *label,
                      GtkWidget *widget)
{
    GtkWidget *name = zg_ui_label_dim(label);

    gtk_grid_attach(GTK_GRID(grid), name, 0, *row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, *row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);

    (*row)++;

    return widget;
}

/* ------------------------------------------------------------------------- */
/* 文本视图                                                                   */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_text_view_new(GtkTextBuffer **buffer_out,
                    gboolean wrap)
{
    GtkWidget *scrolled = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                                wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 6);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), view);

    if (buffer_out != NULL)
        *buffer_out = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

    return scrolled;
}

void zg_ui_text_set(GtkTextBuffer *buffer,
                    const gchar *text)
{
    if (buffer == NULL)
        return;

    gtk_text_buffer_set_text(buffer, text != NULL ? text : "", -1);
}

/* ------------------------------------------------------------------------- */
/* 树视图                                                                     */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_tree_view_new(GtkTreeModel *model)
{
    GtkWidget *view = gtk_tree_view_new_with_model(model);

    gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(view), FALSE);
    gtk_tree_selection_set_mode(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_SINGLE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), 0);
    gtk_widget_add_css_class(view, "zg-tree");

    /* 模型所有权转移给视图 */
    g_object_unref(model);

    return view;
}

GtkTreeViewColumn *
zg_ui_add_column(GtkTreeView *view,
                 const gchar *title,
                 gint column_id,
                 gboolean expand)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column;

    column = gtk_tree_view_column_new_with_attributes(title, renderer,
                                                      "text", column_id, NULL);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_expand(column, expand);
    gtk_tree_view_append_column(view, column);

    return column;
}

/* ------------------------------------------------------------------------- */
/* 状态标签                                                                   */
/* ------------------------------------------------------------------------- */

static void
state_label_apply(GtkWidget *label,
                  const gchar *css_class)
{
    const gchar *current = g_object_get_data(G_OBJECT(label), "zg-state-class");
    guint i;

    if (g_strcmp0(current, css_class) == 0)
        return;

    for (i = 0; i < G_N_ELEMENTS(zg_state_classes); i++)
        gtk_widget_remove_css_class(label, zg_state_classes[i]);

    gtk_widget_add_css_class(label, css_class);
    g_object_set_data(G_OBJECT(label), "zg-state-class", (gpointer)css_class);
}

GtkWidget *
zg_ui_state_label_new(const gchar *state)
{
    GtkWidget *label = gtk_label_new(state != NULL ? state : "—");

    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    state_label_apply(label, zg_model_state_css(state));

    return label;
}

void zg_ui_state_label_set(GtkWidget *label,
                           const gchar *state)
{
    gtk_label_set_text(GTK_LABEL(label), state != NULL ? state : "—");
    state_label_apply(label, zg_model_state_css(state));
}

void zg_ui_label_set_text(GtkWidget *label,
                          const gchar *text)
{
    if (label != NULL)
        gtk_label_set_text(GTK_LABEL(label), text != NULL ? text : "—");
}

/* ------------------------------------------------------------------------- */
/* 提示条                                                                     */
/* ------------------------------------------------------------------------- */

GtkWidget *
zg_ui_banner_new(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    gtk_widget_add_css_class(box, "zg-warning-banner");
    gtk_widget_set_margin_top(box, 4);
    gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    return box;
}

void zg_ui_banner_set(GtkWidget *banner,
                      const gchar *title,
                      const gchar *detail,
                      gboolean warning)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(banner)) != NULL)
        gtk_box_remove(GTK_BOX(banner), child);

    if (warning)
        gtk_widget_add_css_class(banner, "zg-warning-banner");
    else
        gtk_widget_remove_css_class(banner, "zg-warning-banner");

    gtk_box_append(GTK_BOX(banner), zg_ui_label_heading(title));

    if (detail != NULL && detail[0] != '\0')
    {
        GtkWidget *label = zg_ui_label_dim(detail);

        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(banner), label);
    }
}
