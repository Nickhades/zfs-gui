/* zg-window.c - 主窗口（现代化侧边导航布局）
 *
 * 布局：
 *   标题栏：页面标题 / 副标题 + 安全徽章 + 刷新 + 菜单
 *   ├── 侧边栏：品牌区、功能导航、安全状态卡片
 *   └── 内容区：连接提示条 + 页面堆栈（滑动转场）+ 状态栏 + Toast
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-window.h"

#include <string.h>

#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-pages.h"
#include "zg-ui.h"
#include "zg-util.h"

typedef struct
{
    const gchar *icon;
    const gchar *title;
    const gchar *subtitle;
} NavItem;

static gboolean on_shortcut_help_zoom(GtkWidget *widget,
                                      GVariant *arguments,
                                      gpointer user_data);

static const NavItem nav_items[] = {
        {"drive-harddisk-symbolic", "存储池", "容量、健康与 vdev 拓扑"},
        {"folder-symbolic", "数据集", "文件系统、卷与属性"},
        {"camera-photo-symbolic", "快照", "回滚、克隆与发送"},
        {"document-send-symbolic", "发送/接收", "快照流传输与恢复"},
        {"applications-system-symbolic", "启动环境", "bectl 启动环境管理"},
        {"preferences-system-symbolic", "系统", "版本、ARC 与任务日志"},
        {"utilities-terminal-symbolic", "命令中心", "全部受控操作"},
        {"help-browser-symbolic", "操作帮助", "每个功能的详细说明"},
};

typedef struct
{
    ZgApp *app;
    GtkWidget *stack;
    GtkWidget *title_label;
    GtkWidget *subtitle_label;
    GtkWidget *brand_subtitle;
    GtkWidget *secure_box;
    GtkWidget *secure_title;
    GtkWidget *secure_detail;
    GtkWidget *connection_label;
    GtkWidget *nav;
} WindowContext;

/* 当前窗口上下文（单窗口应用）：供导航快捷键回调使用 */
static WindowContext *active_context;

/* ------------------------------------------------------------------------- */
/* 回调                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_minimize_clicked(GtkButton *button,
                    gpointer user_data)
{
    (void)button;

    gtk_window_minimize(GTK_WINDOW(user_data));
}

static void
on_maximize_clicked(GtkButton *button,
                    gpointer user_data)
{
    GtkWindow *window = GTK_WINDOW(user_data);

    (void)button;

    if (gtk_window_is_maximized(window))
        gtk_window_unmaximize(window);
    else
        gtk_window_maximize(window);
}

static void
on_close_clicked(GtkButton *button,
                 gpointer user_data)
{
    (void)button;

    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_quit_clicked(GtkButton *button,
                gpointer user_data)
{
    (void)button;

    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_maximized_changed(GObject *object,
                     GParamSpec *pspec,
                     gpointer user_data)
{
    GtkWidget *image = user_data;

    (void)object;
    (void)pspec;

    gtk_image_set_from_icon_name(GTK_IMAGE(image),
                                 gtk_window_is_maximized(GTK_WINDOW(object))
                                         ? "window-restore-symbolic"
                                         : "window-maximize-symbolic");
}

static void
on_refresh_clicked(GtkButton *button,
                   gpointer user_data)
{
    (void)button;

    zg_app_refresh_all(user_data);
}

static void
on_reconnect_clicked(GtkButton *button,
                     gpointer user_data)
{
    (void)button;

    zg_app_reconnect(user_data);
}

static void
on_about_clicked(GtkButton *button,
                 gpointer user_data)
{
    ZgApp *app = user_data;
    gchar *detail;

    (void)button;

    detail = g_strdup_printf(
            "ZFS 管理工具 1.0.0\n"
            "面向 FreeBSD + OpenZFS 的 zpool / zfs / bectl 管理界面\n\n"
            "主机：%s\n"
            "系统：%s\n"
            "ZFS 用户态：%s\n"
            "ZFS 内核：%s\n"
            "后台服务：%s\n"
            "socket：%s\n"
            "认证策略：%s\n\n"
            "安全模型\n"
            "· 前端不直接执行命令，全部操作经特权守护进程白名单校验\n"
            "· 危险操作必须重新验证管理员密码（PAM）\n"
            "· 命令以 argv 方式执行，不经过 shell\n"
            "· 认证失败会触发临时锁定",
            zg_str_or(zg_app_hostname(app), "—"),
            zg_str_or(zg_app_osrelease(app), "—"),
            zg_str_or(zg_app_zfs_userland(app), "—"),
            zg_str_or(zg_app_zfs_kernel(app), "—"),
            zg_str_or(zg_app_daemon_version(app), "未连接"),
            zg_str_or(zg_app_socket_path(app), "—"),
            zg_app_auth_mode(app));

    zg_dialogs_info(zg_app_get_window(app), "关于 ZFS 管理工具", detail);
    g_free(detail);
}

static gboolean
on_shortcut_nav(GtkWidget *widget,
                GVariant *arguments,
                gpointer user_data)
{
    gint index = GPOINTER_TO_INT(user_data);
    GtkListBoxRow *row;

    (void)widget;
    (void)arguments;

    if (active_context == NULL || active_context->nav == NULL)
        return FALSE;

    row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(active_context->nav), index);

    if (row != NULL)
        gtk_list_box_select_row(GTK_LIST_BOX(active_context->nav), row);

    return TRUE;
}

static gboolean
on_shortcut_help_zoom(GtkWidget *widget,
                      GVariant *arguments,
                      gpointer user_data)
{
    (void)widget;
    (void)arguments;

    return zg_page_help_zoom(GPOINTER_TO_INT(user_data));
}

static gboolean
on_shortcut_quit(GtkWidget *widget,
                 GVariant *arguments,
                 gpointer user_data)
{
    (void)widget;
    (void)arguments;
    (void)user_data;

    gtk_window_close(zg_app_get_window(zg_app_get_default()));

    return TRUE;
}

static gboolean
on_shortcut_refresh(GtkWidget *widget,
                    GVariant *arguments,
                    gpointer user_data)
{
    (void)widget;
    (void)arguments;
    (void)user_data;

    zg_app_refresh_all(zg_app_get_default());

    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* 导航                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_nav_row_selected(GtkListBox *list_box,
                    GtkListBoxRow *row,
                    gpointer user_data)
{
    WindowContext *context = user_data;
    gint index;

    (void)list_box;

    if (row == NULL)
        return;

    index = gtk_list_box_row_get_index(row);

    if (index < 0 || index >= (gint)G_N_ELEMENTS(nav_items))
        return;

    {
        gchar *name = g_strdup_printf("page-%d", index);

        gtk_stack_set_visible_child_name(GTK_STACK(context->stack), name);
        g_free(name);
    }

    gtk_label_set_text(GTK_LABEL(context->title_label), nav_items[index].title);
    gtk_label_set_text(GTK_LABEL(context->subtitle_label), nav_items[index].subtitle);
}

static GtkWidget *
build_sidebar(WindowContext *context,
              GtkWidget **out_nav_list)
{
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *brand;
    GtkWidget *brand_icon;
    GtkWidget *brand_texts;
    GtkWidget *brand_title;
    GtkWidget *nav;
    GtkWidget *footer;
    GtkWidget *secure_row;
    GtkWidget *secure_icon;
    GtkWidget *separator;
    guint i;

    gtk_widget_add_css_class(sidebar, "zg-sidebar");
    gtk_widget_set_size_request(sidebar, 214, -1);

    /* 品牌区 */
    brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(brand, 18);
    gtk_widget_set_margin_bottom(brand, 14);
    gtk_widget_set_margin_start(brand, 16);
    gtk_widget_set_margin_end(brand, 16);

    brand_icon = gtk_image_new_from_icon_name("drive-harddisk-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(brand_icon), 26);
    gtk_widget_add_css_class(brand_icon, "zg-brand-tile");

    brand_texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    brand_title = gtk_label_new("ZFS 管理");
    gtk_label_set_xalign(GTK_LABEL(brand_title), 0.0);
    gtk_widget_add_css_class(brand_title, "zg-brand-title");

    context->brand_subtitle = zg_ui_label_dim("正在连接…");
    gtk_widget_add_css_class(context->brand_subtitle, "zg-brand-sub");
    gtk_box_append(GTK_BOX(brand_texts), brand_title);
    gtk_box_append(GTK_BOX(brand_texts), context->brand_subtitle);

    gtk_box_append(GTK_BOX(brand), brand_icon);
    gtk_box_append(GTK_BOX(brand), brand_texts);
    gtk_box_append(GTK_BOX(sidebar), brand);

    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_start(separator, 16);
    gtk_widget_set_margin_end(separator, 16);
    gtk_widget_set_margin_bottom(separator, 8);
    gtk_box_append(GTK_BOX(sidebar), separator);

    /* 导航 */
    nav = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(nav), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(nav, "zg-nav-list");
    gtk_widget_set_margin_start(nav, 4);
    gtk_widget_set_margin_end(nav, 4);

    for (i = 0; i < G_N_ELEMENTS(nav_items); i++)
    {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *icon = gtk_image_new_from_icon_name(nav_items[i].icon);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *title = gtk_label_new(nav_items[i].title);
        GtkWidget *subtitle = gtk_label_new(nav_items[i].subtitle);

        gtk_widget_set_margin_top(box, 7);
        gtk_widget_set_margin_bottom(box, 7);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);

        gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_widget_add_css_class(title, "zg-nav-title");
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
        gtk_widget_add_css_class(subtitle, "zg-nav-subtitle");

        gtk_box_append(GTK_BOX(texts), title);
        gtk_box_append(GTK_BOX(texts), subtitle);

        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), texts);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_widget_add_css_class(row, "zg-sidebar-row");
        gtk_list_box_append(GTK_LIST_BOX(nav), row);
    }

    g_signal_connect(nav, "row-selected", G_CALLBACK(on_nav_row_selected), context);
    gtk_box_append(GTK_BOX(sidebar), nav);
    context->nav = nav;

    if (out_nav_list != NULL)
        *out_nav_list = nav;

    /* 底部：安全状态卡片 */
    footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(footer, GTK_ALIGN_END);
    gtk_widget_set_vexpand(footer, TRUE);
    gtk_widget_set_margin_bottom(footer, 14);
    gtk_widget_set_margin_start(footer, 10);
    gtk_widget_set_margin_end(footer, 10);

    context->secure_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(context->secure_box, "zg-secure-badge");

    secure_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    secure_icon = gtk_image_new_from_icon_name("channel-secure-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(secure_icon), 16);
    gtk_widget_add_css_class(secure_icon, "zg-lock-icon");

    context->secure_title = zg_ui_label("受保护");
    gtk_widget_add_css_class(context->secure_title, "zg-secure-title");

    gtk_box_append(GTK_BOX(secure_row), secure_icon);
    gtk_box_append(GTK_BOX(secure_row), context->secure_title);

    context->secure_detail = zg_ui_label_dim("危险操作需要密码");
    gtk_widget_add_css_class(context->secure_detail, "zg-secure-detail");
    /* 单行省略：侧栏宽度固定，避免换行后高度计算不准与下面的胶囊重叠 */
    gtk_label_set_ellipsize(GTK_LABEL(context->secure_detail), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(context->secure_detail), TRUE);

    context->connection_label = zg_ui_label("未连接");
    gtk_widget_add_css_class(context->connection_label, "zg-pill");
    gtk_widget_add_css_class(context->connection_label, "zg-pill-grey");
    gtk_widget_set_halign(context->connection_label, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(context->secure_box), secure_row);
    gtk_box_append(GTK_BOX(context->secure_box), context->secure_detail);
    gtk_box_append(GTK_BOX(context->secure_box), context->connection_label);

    gtk_box_append(GTK_BOX(footer), context->secure_box);
    gtk_box_append(GTK_BOX(sidebar), footer);

    return sidebar;
}

/* ------------------------------------------------------------------------- */
/* 状态同步                                                                   */
/* ------------------------------------------------------------------------- */

static void
set_pill(GtkWidget *label,
         const gchar *text,
         const gchar *css_class)
{
    gtk_label_set_text(GTK_LABEL(label), text);

    if (g_strcmp0(g_object_get_data(G_OBJECT(label), "zg-pill-class"), css_class) == 0)
        return;

    gtk_widget_remove_css_class(label, "zg-pill-green");
    gtk_widget_remove_css_class(label, "zg-pill-red");
    gtk_widget_remove_css_class(label, "zg-pill-grey");
    gtk_widget_add_css_class(label, css_class);
    g_object_set_data(G_OBJECT(label), "zg-pill-class", (gpointer)css_class);
}

static void
update_chrome(gpointer data)
{
    WindowContext *context = data;
    ZgApp *app = context->app;
    const gchar *mode = zg_app_auth_mode(app);
    gchar *text;

    if (!zg_app_is_connected(app))
    {
        gtk_label_set_text(GTK_LABEL(context->brand_subtitle), "未连接后台服务");
        set_pill(context->connection_label, "未连接", "zg-pill-red");
        return;
    }

    text = g_strdup_printf("%s · %s",
                           zg_str_or(zg_app_hostname(app), "本机"),
                           zg_str_or(zg_app_osrelease(app), "FreeBSD"));
    gtk_label_set_text(GTK_LABEL(context->brand_subtitle), text);
    g_free(text);

    if (zg_app_daemon_euid(app) != 0)
    {
        gtk_label_set_text(GTK_LABEL(context->secure_title), "后台服务未提权");
        gtk_label_set_text(GTK_LABEL(context->secure_detail),
                           "密码无法验证，写操作会失败");
        gtk_widget_set_tooltip_text(context->secure_detail,
                                    "后台服务未以 root 运行：无法验证管理员密码，"
                                    "创建 / 修改 / 销毁等操作会失败。"
                                    "请以 root 启动：service zfsguid start");
        gtk_widget_remove_css_class(context->secure_box, "zg-secure-badge");
        gtk_widget_add_css_class(context->secure_box, "zg-insecure-badge");
        set_pill(context->connection_label, "已连接（无特权）", "zg-pill-red");
        return;
    }

    set_pill(context->connection_label, "已连接", "zg-pill-green");

    if (g_strcmp0(mode, "none") == 0)
    {
        gtk_label_set_text(GTK_LABEL(context->secure_title), "二次认证未启用");
        gtk_label_set_text(GTK_LABEL(context->secure_detail),
                           "auth_mode = none：危险操作不会要求密码");
        gtk_widget_remove_css_class(context->secure_box, "zg-secure-badge");
        gtk_widget_add_css_class(context->secure_box, "zg-insecure-badge");
    }
    else if (g_strcmp0(mode, "all") == 0)
    {
        gtk_label_set_text(GTK_LABEL(context->secure_title), "严格模式");
        gtk_label_set_text(GTK_LABEL(context->secure_detail),
                           "auth_mode = all：所有写操作都需要管理员密码");
        gtk_widget_remove_css_class(context->secure_box, "zg-insecure-badge");
        gtk_widget_add_css_class(context->secure_box, "zg-secure-badge");
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(context->secure_title), "受保护");
        gtk_label_set_text(GTK_LABEL(context->secure_detail),
                           "危险操作需管理员密码（PAM 验证）");
        gtk_widget_remove_css_class(context->secure_box, "zg-insecure-badge");
        gtk_widget_add_css_class(context->secure_box, "zg-secure-badge");
    }
}

/* ------------------------------------------------------------------------- */
/* 主窗口                                                                     */
/* ------------------------------------------------------------------------- */

static GtkWidget *
build_menu_button(ZgApp *app)
{
    GtkWidget *button = gtk_menu_button_new();
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(button, "菜单");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);

    {
        GtkWidget *items[] = {
                zg_ui_button("刷新全部页面  (Ctrl+R)", G_CALLBACK(on_refresh_clicked), app),
                zg_ui_button("重新连接后台服务", G_CALLBACK(on_reconnect_clicked), app),
                zg_ui_button("关于 ZFS 管理工具", G_CALLBACK(on_about_clicked), app),
                zg_ui_button("退出  (Ctrl+Q)", G_CALLBACK(on_quit_clicked), app),
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS(items); i++)
        {
            gtk_button_set_has_frame(GTK_BUTTON(items[i]), FALSE);
            gtk_widget_set_halign(items[i], GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(box), items[i]);
        }
    }

    gtk_popover_set_child(GTK_POPOVER(popover), box);

    return button;
}

GtkWidget *
zg_window_new(ZgApp *app)
{
    WindowContext *context = g_new0(WindowContext, 1);
    GtkWidget *window;
    GtkWidget *header;
    GtkWidget *title_box;
    GtkWidget *refresh_button;
    GtkWidget *menu_button;
    GtkWidget *content;
    GtkWidget *main_area;
    GtkWidget *stack;
    GtkWidget *sidebar;
    GtkWidget *nav_list = NULL;
    GtkWidget *banner;
    GtkWidget *banner_revealer;
    GtkWidget *statusbar;
    GtkWidget *status_label;
    GtkWidget *socket_label;
    GtkWidget *overlay;
    GtkWidget *toast_revealer;
    GtkWidget *toast_label;
    GtkEventController *shortcuts;

    context->app = app;

    window = gtk_application_window_new(zg_app_get_application(app));
    gtk_window_set_title(GTK_WINDOW(window), "ZFS 管理工具");
    gtk_window_set_default_size(GTK_WINDOW(window), 1500, 940);

    /* ---- 标题栏 ---- */
    header = gtk_header_bar_new();

    /* 不同主题（尤其 Breeze）可能隐藏 GTK 的窗口按钮，导致无法最小化/关闭。
     * 这里关闭系统按钮，改用自绘按钮，保证任何主题下都能操作窗口。 */
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), FALSE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);

    context->title_label = gtk_label_new("存储池");
    gtk_label_set_xalign(GTK_LABEL(context->title_label), 0.0);
    gtk_widget_add_css_class(context->title_label, "zg-page-title");

    context->subtitle_label = zg_ui_label_dim("容量、健康与 vdev 拓扑");
    gtk_label_set_xalign(GTK_LABEL(context->subtitle_label), 0.0);

    gtk_box_append(GTK_BOX(title_box), context->title_label);
    gtk_box_append(GTK_BOX(title_box), context->subtitle_label);
    gtk_widget_add_css_class(context->subtitle_label, "zg-page-subtitle");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), title_box);

    /* 用空标题控件抑制 HeaderBar 居中显示窗口标题 */
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header),
                                    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

    {
        GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        GtkWidget *minimize = gtk_button_new_from_icon_name("window-minimize-symbolic");
        GtkWidget *maximize = gtk_button_new_from_icon_name("window-maximize-symbolic");
        GtkWidget *close_button = gtk_button_new_from_icon_name("window-close-symbolic");

        gtk_widget_add_css_class(minimize, "flat");
        gtk_widget_add_css_class(minimize, "zg-window-control");
        gtk_widget_set_tooltip_text(minimize, "最小化");

        gtk_widget_add_css_class(maximize, "flat");
        gtk_widget_add_css_class(maximize, "zg-window-control");
        gtk_widget_set_tooltip_text(maximize, "最大化 / 还原");

        gtk_widget_add_css_class(close_button, "flat");
        gtk_widget_add_css_class(close_button, "zg-window-control");
        gtk_widget_add_css_class(close_button, "zg-window-close");
        gtk_widget_set_tooltip_text(close_button, "关闭窗口");

        g_signal_connect(minimize, "clicked", G_CALLBACK(on_minimize_clicked), window);
        g_signal_connect(maximize, "clicked", G_CALLBACK(on_maximize_clicked), window);
        g_signal_connect(close_button, "clicked", G_CALLBACK(on_close_clicked), window);
        g_signal_connect(window, "notify::maximized",
                         G_CALLBACK(on_maximized_changed),
                         gtk_button_get_child(GTK_BUTTON(maximize)));

        gtk_box_append(GTK_BOX(controls), minimize);
        gtk_box_append(GTK_BOX(controls), maximize);
        gtk_box_append(GTK_BOX(controls), close_button);

        gtk_header_bar_pack_end(GTK_HEADER_BAR(header), controls);
    }

    refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button, "刷新全部页面 (Ctrl+R)");
    gtk_widget_add_css_class(refresh_button, "suggested-action");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_button);

    menu_button = build_menu_button(app);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), menu_button);

    /* ---- 主体 ---- */
    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    banner = zg_ui_banner_new();
    banner_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(banner_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(banner_revealer), banner);
    gtk_revealer_set_reveal_child(GTK_REVEALER(banner_revealer), FALSE);
    gtk_box_append(GTK_BOX(content), banner_revealer);

    main_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(main_area, TRUE);

    stack = gtk_stack_new();
    context->stack = stack;
    /* 页面切换使用即时切换：GTK 的滑动/淡入转场在部分主题下会卡住，
     * 导致两个页面同时可见。即时切换更可靠，也更快。 */
    gtk_stack_set_transition_type(GTK_STACK(stack),
                                  GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);

    {
        GtkWidget *pages[] = {
                zg_page_pools_new(app),
                zg_page_datasets_new(app),
                zg_page_snapshots_new(app),
                zg_page_sendrecv_new(app),
                zg_page_be_new(app),
                zg_page_system_new(app),
                zg_page_ops_new(app),
                zg_page_help_new(app),
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS(pages); i++)
        {
            gchar *name = g_strdup_printf("page-%u", i);

            gtk_stack_add_named(GTK_STACK(stack), pages[i], name);
            g_free(name);
        }
    }

    sidebar = build_sidebar(context, &nav_list);

    gtk_box_append(GTK_BOX(main_area), sidebar);
    gtk_box_append(GTK_BOX(main_area), stack);
    gtk_box_append(GTK_BOX(content), main_area);

    /* ---- 状态栏 ---- */
    statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(statusbar, "zg-statusbar");

    status_label = zg_ui_label("就绪");
    gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(status_label), 1);
    gtk_label_set_max_width_chars(GTK_LABEL(status_label), 90);
    gtk_widget_set_hexpand(status_label, TRUE);

    socket_label = zg_ui_label_dim(zg_client_socket_path());
    gtk_label_set_ellipsize(GTK_LABEL(socket_label), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(statusbar), status_label);
    gtk_box_append(GTK_BOX(statusbar), socket_label);
    gtk_box_append(GTK_BOX(content), statusbar);

    /* ---- Toast 覆盖层 ---- */
    overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), content);

    toast_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(toast_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_widget_set_halign(toast_revealer, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(toast_revealer, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(toast_revealer, 44);

    {
        GtkWidget *toast_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        gtk_widget_add_css_class(toast_box, "zg-toast");
        toast_label = gtk_label_new("");
        gtk_box_append(GTK_BOX(toast_box), toast_label);
        gtk_revealer_set_child(GTK_REVEALER(toast_revealer), toast_box);
    }

    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toast_revealer);
    gtk_window_set_child(GTK_WINDOW(window), overlay);

    /* ---- 快捷键 ---- */
    shortcuts = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(shortcuts),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>r"),
                             gtk_callback_action_new(on_shortcut_refresh, NULL, NULL)));
    gtk_shortcut_controller_add_shortcut(
            GTK_SHORTCUT_CONTROLLER(shortcuts),
            gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>q"),
                             gtk_callback_action_new(on_shortcut_quit, NULL, NULL)));

    /* Alt+1..6 直接跳转到对应功能页 */
    {
        guint i;

        for (i = 0; i < G_N_ELEMENTS(nav_items); i++)
        {
            gchar *accelerator = g_strdup_printf("<Alt>%u", i + 1);

            gtk_shortcut_controller_add_shortcut(
                    GTK_SHORTCUT_CONTROLLER(shortcuts),
                    gtk_shortcut_new(gtk_shortcut_trigger_parse_string(accelerator),
                                     gtk_callback_action_new(on_shortcut_nav,
                                                             GINT_TO_POINTER((gint)i),
                                                             NULL)));
            g_free(accelerator);
        }
    }
    /* Ctrl+加号/减号/0：帮助页字号缩放 */
    {
        static const struct
        {
            const gchar *trigger;
            gint delta;
        } items[] = {
                {"<Control>plus", 10},
                {"<Control>equal", 10},
                {"<Control>minus", -10},
                {"<Control>0", 0},
        };
        guint i;

        for (i = 0; i < G_N_ELEMENTS(items); i++)
        {
            GtkShortcutTrigger *trigger = gtk_shortcut_trigger_parse_string(items[i].trigger);

            if (trigger == NULL)
                continue;

            gtk_shortcut_controller_add_shortcut(
                    GTK_SHORTCUT_CONTROLLER(shortcuts),
                    gtk_shortcut_new(trigger,
                                     gtk_callback_action_new(on_shortcut_help_zoom,
                                                             GINT_TO_POINTER(items[i].delta),
                                                             NULL)));
        }
    }

    gtk_widget_add_controller(window, shortcuts);

    /* ---- 与 App 关联 ---- */
    zg_app_set_window(app, GTK_WINDOW(window));
    zg_app_set_status_widget(app, status_label);
    zg_app_set_banner_widget(app, banner_revealer, banner);
    zg_app_set_toast_widget(app, toast_revealer, toast_label);

    g_object_set_data_full(G_OBJECT(window), "zg-window-context", context, g_free);
    active_context = context;

    zg_app_register_refresh(app, update_chrome, context);
    update_chrome(context);

    if (nav_list != NULL)
    {
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav_list),
                                                             g_getenv("ZFS_GUI_START_COMMAND") != NULL
                                                                     ? (gint)G_N_ELEMENTS(nav_items) - 1
                                                                     : 0);

        if (first != NULL)
            gtk_list_box_select_row(GTK_LIST_BOX(nav_list), first);
    }

    return window;
}
