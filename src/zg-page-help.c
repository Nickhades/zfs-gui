/* zg-page-help.c - 操作帮助页面
 *
 * 面向普通操作者的中文使用说明：按页面/主题组织，并自动汇总全部受控操作。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-pages.h"

#include <string.h>

#include "zg-app.h"
#include "zg-opdef.h"
#include "zg-ui.h"

typedef struct
{
    const gchar *group;
    const gchar *title;
    const gchar *body;
} HelpTopic;

static const HelpTopic topics[] = {
        {
                "开始",
                "连接、刷新与权限",
                "【连接后台服务】\n"
                "界面所有读写都通过本机后台服务 zfs-guid 完成（socket 默认 "
                "/var/run/zfs-gui/zfs-gui.sock）。左下角显示「已连接」即正常；"
                "服务未启动或权限不足时顶部会出现黄色提示条，界面每 3 秒自动重试。\n\n"
                "【刷新】\n"
                "F5 或 Ctrl+R、右上角刷新按钮、或菜单「刷新全部页面」都会重新读取数据。\n"
                "存储池与数据集的容量、健康状态还会每 10 秒自动更新一次，不会打断当前选择。\n\n"
                "【权限】\n"
                "普通用户需要：在 wheel 组，或在 /usr/local/etc/zfs-gui.conf 的 "
                "allowed_users / allowed_groups 中；socket 目录必须可进入（0711）、"
                "socket 为 0660。连接失败时界面会给出权限提示。\n\n"
                "【危险操作】\n"
                "会修改状态的操作先弹确认框；危险操作（销毁、回滚、覆盖导入等）还需要"
                "输入管理员密码，通过 PAM 重新验证，连续失败会被短暂锁定。",
        },
        {
                "页面指南",
                "存储池",
                "【列表】\n"
                "左侧是「池 → vdev → 磁盘」树：点池查看概况与操作，点磁盘查看设备信息。\n\n"
                "【工具栏】\n"
                "• 创建存储池：向导中选择拓扑、勾选磁盘（已在使用中的磁盘会自动禁用），"
                "勾选「强制创建 (-f)」可覆盖磁盘上已有的分区/文件系统。\n"
                "• 导入存储池：扫描可导入的池；外置硬盘若是 NTFS/ext4/exFAT 等文件系统，"
                "ZFS 无法直接导入，可点「格式化磁盘为 ZFS（新建池）」先格式化。\n"
                "• 刷新：重新读取池列表。\n"
                "• 系统快照：一键对整个池执行 zfs snapshot -r <pool>@system-时间戳，"
                "递归包含全部数据集，完成后提示并自动刷新。\n\n"
                "【对象面板】\n"
                "属性、状态详情、添加磁盘、校验 scrub、TRIM；「更多操作」里还有清除错误、"
                "维护、导出、销毁。\n\n"
                "【维护窗口】\n"
                "包含升级池版本、初始化空闲块、重建冗余、创建/丢弃检查点、预取元数据、"
                "同步数据、重新打开设备。每项执行前都会再次确认。\n\n"
                "【自动刷新】\n"
                "每 10 秒更新容量、可用空间、碎片率与健康状态，只更新数字，不重置选择。",
        },
        {
                "页面指南",
                "数据集",
                "【列表】\n"
                "按层级显示文件系统与卷，展开箭头查看子数据集；右侧面板显示属性。\n\n"
                "【创建数据集】\n"
                "工具栏「创建数据集」，填写名称（可含 / 表示层级）与挂载点等属性。\n\n"
                "【对象面板】\n"
                "• 属性：查看/编辑 ZFS 属性。\n"
                "• 创建快照：可为单个数据集创建快照（勾选递归则包含子数据集）。\n"
                "• 挂载 / 卸载：切换 mountpoint 的挂载状态。\n"
                "• 重命名：修改数据集名称。\n"
                "• 配额：设置 quota / refquota 等限制。\n"
                "• 更多操作：销毁、发送、克隆等。\n\n"
                "【右侧属性页】\n"
                "「属性 / 用户配额 / 组配额」三个页签，可刷新与编辑；修改后会先确认。\n\n"
                "【快捷键】\n"
                "Enter 打开属性，F2 重命名，Delete 销毁，F5 刷新。",
        },
        {
                "页面指南",
                "快照",
                "【创建快照】\n"
                "选中数据集后点「创建快照」，名称不要包含 @；勾选「递归创建 (-r，包含全部"
                "子数据集)」即可为整个系统（如 zroot）创建一套完整快照。\n\n"
                "【独占空间为什么是 0B】\n"
                "ZFS 快照是写时复制：新快照与当前数据共用数据块，所以「独占空间」为 0B 属正常。"
                "「包含数据」才是它能恢复的数据总量。之后数据被改写/删除，独占空间才会增长。\n\n"
                "【操作】\n"
                "• 发送：把快照导出为 .zfs 备份文件。\n"
                "• 回滚：恢复到该时间点，会丢弃之后的数据。\n"
                "• 克隆：从快照创建可写数据集，保留当前数据不动。\n"
                "• 差异：显示快照与当前数据的文件差异。\n"
                "• hold：加保持标签，防止被误销毁。\n"
                "• 销毁：删除快照（危险操作，需要管理员密码）。",
        },
        {
                "页面指南",
                "发送 / 接收",
                "【发送快照到文件】\n"
                "• 源快照：自动探测系统全部快照，可输入名称搜索，右侧按钮可重新扫描。\n"
                "• 输出文件：可直接输入路径，或点右侧文件夹图标打开系统文件选择器，"
                "选择本机目录或已挂载的外接硬盘。\n"
                "• 模式：全量发送；增量 (-i) 需起始快照；复制 (-R，含属性与后代) 用于"
                "整个系统的完整备份；增量含中间快照 (-I)。\n"
                "• 可勾选压缩数据流 (-c) 与仅演练 (-n)。\n\n"
                "【备份进度】\n"
                "开始发送后会打开「快照备份」窗口：显示目标文件、已写入大小（实时刷新）、"
                "用时、滚动日志；完成后按钮变为可关闭，并弹出成功提示、自动刷新页面。\n\n"
                "【从文件接收】\n"
                "选择 .zfs 文件与目标数据集；可勾选强制接收 (-F)、接收后不挂载 (-u)、"
                "仅演练 (-n)。\n\n"
                "【命令行等价】\n"
                "zfs snapshot -r zroot@full-20261108\n"
                "zfs send -R zroot@full-20261108 > /backup/zroot-full.zfs\n"
                "zfs receive -F -d tank < /backup/zroot-full.zfs",
        },
        {
                "页面指南",
                "启动环境（bectl）",
                "【用途】\n"
                "FreeBSD 启动环境用于在多个根文件系统快照之间切换，升级失败时可回退。\n\n"
                "【操作】\n"
                "• 列表：查看现有启动环境及当前激活项。\n"
                "• 创建：为当前环境创建快照型启动环境。\n"
                "• 激活：设为下次启动项，需要重启生效。\n"
                "• 挂载 / 卸载：临时挂载启动环境以检查文件。\n"
                "• 重命名 / 销毁：修改名称或删除（销毁属于危险操作）。",
        },
        {
                "页面指南",
                "系统",
                "【概览】\n"
                "显示 ZFS 版本、内核版本与基本统计。\n\n"
                "【ARC】\n"
                "查看 ARC 命中率、缓存大小、各状态列表，用于判断内存是否足够。\n\n"
                "【任务日志】\n"
                "最近执行过的操作及其结果，失败原因会记录在这里。\n\n"
                "【命令中心】\n"
                "需要执行本页未覆盖的高级命令时，从左侧「命令中心」进入，"
                "按类别查找并双击打开表单。",
        },
        {
                "页面指南",
                "命令中心",
                "【使用方式】\n"
                "顶部下拉按类别筛选（存储池、数据集、快照、属性、发送/接收、加密、委派、"
                "启动环境、系统），搜索框可按名称、ID 或说明过滤。\n\n"
                "【执行】\n"
                "双击条目打开表单；表单会实时显示最终命令，填写参数后提交。"
                "危险操作仍会要求确认或管理员密码。\n\n"
                "【安全】\n"
                "界面不能拼接任意命令：只能从登记表中选操作、开关、选项与值，"
                "由后台以 execv 执行。未登记的参数会被拒绝。",
        },
        {
                "安全",
                "认证、白名单与锁定",
                "【三级权限】\n"
                "• 只读：查询类操作，无需确认。\n"
                "• 确认：会修改状态，弹确认框即可。\n"
                "• 密码：危险操作，需要管理员密码（PAM 重新验证）。\n\n"
                "【PAM】\n"
                "默认使用 /etc/pam.d/zfs-gui，缺失时回退 system。只有 root 或允许的"
                "管理用户/组可以通过验证。\n\n"
                "【失败锁定】\n"
                "连续输错密码会在 auth_lockout 秒内锁定该用户，避免暴力尝试。\n\n"
                "【配置文件 /usr/local/etc/zfs-gui.conf】\n"
                "allowed_users / allowed_groups：允许连接与认证的用户和组；\n"
                "auth_mode = dangerous（默认）/ all / none；\n"
                "auth_max_attempts、auth_lockout；socket_group、socket_mode。\n\n"
                "【非 root 守护进程】\n"
                "若以普通用户运行 zfs-guid，界面会显示黄色警告，创建/修改/销毁类操作"
                "将无法通过密码验证。请用 service zfsguid start 以 root 启动。",
        },
        {
                "许可",
                "开源许可与二次分发",
                "【许可证】\n"
                "本项目采用 BSD 2-Clause（简化 BSD）许可证：允许使用、修改、分发、"
                "商业使用与闭源衍生，不强制开源修改后的代码。\n\n"
                "【分发义务】\n"
                "只需保留版权声明、许可证文本与免责声明：\n"
                "• 源码分发：在源码中保留上述声明；\n"
                "• 二进制分发：在文档或关于页中复现上述声明。\n"
                "不要求公开修改、不要求相同许可证、不要求提供安装信息。\n\n"
                "【商业使用】\n"
                "允许收费、预装、SaaS 与闭源发布，无需向原作者付费或开源。\n\n"
                "【第三方组件】\n"
                "GTK/GLib 为 LGPL，OpenZFS 工具为 CDDL；本项目仅以独立进程调用 ZFS 工具，"
                "不链接、不复制其代码。\n\n"
                "【更多】\n"
                "完整条款见仓库 LICENSE，分发与合规清单见 OPEN-SOURCE.md。",
        },
        {
                "效率",
                "键盘快捷键",
                "Enter      打开所选对象的属性 / 主操作\n"
                "Delete     销毁所选对象\n"
                "F2         重命名\n"
                "F5         刷新当前页面\n"
                "Ctrl+R     刷新全部页面\n"
                "Ctrl+Q     退出\n"
                "Alt+1..8   直接切换到对应功能页（含操作帮助）\n\n"
                "在「操作帮助」页，直接在搜索框输入关键词即可过滤主题；\n"
                "在「发送/接收」页，源快照下拉框支持输入名称过滤。",
        },
        {
                "常见问题",
                "遇到问题先看这里",
                "【快照显示 0B】\n"
                "正常。0B 是「独占空间」，不是快照内容大小；「包含数据」才是数据量。\n\n"
                "【删了文件容量没变】\n"
                "界面每 10 秒自动刷新；ZFS 释放空间要等事务组提交，可能需要几秒到几十秒。\n\n"
                "【外置硬盘导入不到】\n"
                "只有 ZFS 池能直接导入。NTFS/ext4/exFAT 等需先在「导入存储池」里"
                "点「格式化磁盘为 ZFS（新建池）」格式化（会清空数据）。\n\n"
                "【连接失败 / 权限不足】\n"
                "确认 service zfsguid status 正在运行；当前用户在 wheel 或 allowed_users；"
                "/var/run/zfs-gui 目录可进入（0711）、socket 为 0660。\n\n"
                "【点了没反应】\n"
                "确认后台服务已连接；危险操作需要输入管理员密码；部分操作需要 root 守护进程。\n\n"
                "【界面字体/菜单异常】\n"
                "程序固定使用 Adwaita 浅色主题并全局加粗；如仍有显示异常，可尝试重启界面。",
        },
};

typedef struct
{
    ZgApp *app;
    GtkWidget *list;
    GtkTextBuffer *buffer;
    GtkSearchEntry *search;
    GtkCssProvider *zoom_css;
    gint zoom;
    gchar *reference;
} HelpPage;

#define HELP_TOPIC_COUNT ((gint)G_N_ELEMENTS(topics))
#define HELP_REFERENCE_INDEX HELP_TOPIC_COUNT

/* 帮助页只有一个实例，供窗口级快捷键调用 */
static HelpPage *help_instance;

static gchar *
build_operations_reference(void)
{
    GString *text = g_string_new(
            "以下是全部受控操作（与「命令中心」一一对应），按类别排列。\n"
            "危险等级：只读 / 需确认 / 需管理员密码。\n\n");
    gint category;

    for (category = 0; category < ZG_CAT_COUNT; category++)
    {
        gboolean any = FALSE;
        guint index;

        for (index = 0; index < zg_op_count(); index++)
        {
            const ZgOpDef *op = zg_op_at(index);

            if (op == NULL || (gint)op->category != category)
                continue;

            if (!any)
            {
                g_string_append_printf(text, "【%s】\n",
                                       zg_op_category_label(op->category));
                any = TRUE;
            }

            g_string_append_printf(text, "• %s（%s）\n    %s\n    危险等级：%s\n\n",
                                   op->title, op->id, op->description,
                                   zg_auth_level_label(op->auth));
        }
    }

    return g_string_free(text, FALSE);
}

static const gchar *
help_topic_body(HelpPage *page,
                gint index)
{
    if (index == HELP_REFERENCE_INDEX)
        return page->reference;

    if (index >= 0 && index < HELP_TOPIC_COUNT)
        return topics[index].body;

    return "";
}

static void
on_help_row_selected(GtkListBox *box,
                     GtkListBoxRow *row,
                     gpointer user_data)
{
    HelpPage *page = user_data;
    gint index;
    const gchar *body;

    if (row == NULL)
        return;

    index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "topic-index"));
    body = help_topic_body(page, index);
    gtk_text_buffer_set_text(page->buffer, body, -1);

    (void)box;
}

static void
on_help_search_changed(GtkSearchEntry *entry,
                       gpointer user_data)
{
    HelpPage *page = user_data;
    const gchar *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    GtkWidget *row;
    GtkWidget *first = NULL;
    gboolean has_query = query != NULL && query[0] != '\0';

    for (row = gtk_widget_get_first_child(page->list); row != NULL;
         row = gtk_widget_get_next_sibling(row))
    {
        gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "topic-index"));
        const gchar *title = (index == HELP_REFERENCE_INDEX)
                                     ? "完整操作参考"
                                     : topics[index].title;
        const gchar *body = help_topic_body(page, index);
        gboolean match = TRUE;

        if (has_query)
        {
            match = g_str_match_string(query, title, TRUE) ||
                    (body != NULL && g_str_match_string(query, body, TRUE));
        }

        gtk_widget_set_visible(row, match);

        if (match && first == NULL)
            first = row;
    }

    if (first != NULL)
    {
        GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(page->list));

        if (selected == NULL || !gtk_widget_get_visible(GTK_WIDGET(selected)))
            gtk_list_box_select_row(GTK_LIST_BOX(page->list), GTK_LIST_BOX_ROW(first));
    }
}

#define HELP_ZOOM_MIN 70
#define HELP_ZOOM_MAX 200
#define HELP_ZOOM_STEP 10

static void
help_apply_zoom(HelpPage *page)
{
    gdouble factor = (gdouble)page->zoom / 100.0;
    gchar *css;

    css = g_strdup_printf(
            ".zg-help-text text { font-size: %.1fpx; }\n"
            ".zg-help-text { font-size: %.1fpx; }\n"
            ".zg-help-list row label { font-size: %.1fpx; }\n"
            ".zg-help-list .zg-dim { font-size: %.1fpx; }\n",
            15.0 * factor, 15.0 * factor,
            14.0 * factor, 13.0 * factor);

    gtk_css_provider_load_from_data(page->zoom_css, css, -1);
    g_free(css);
}

static void
help_set_zoom(HelpPage *page,
              gint delta)
{
    gint value;

    if (delta == 0)
        value = 100;
    else
        value = page->zoom + delta;

    if (value < HELP_ZOOM_MIN)
        value = HELP_ZOOM_MIN;

    if (value > HELP_ZOOM_MAX)
        value = HELP_ZOOM_MAX;

    page->zoom = value;
    help_apply_zoom(page);

    zg_app_toast(page->app, "帮助字号：%d%%", page->zoom);
}

gboolean
zg_page_help_zoom(gint delta)
{
    if (help_instance == NULL ||
        !gtk_widget_get_mapped(help_instance->list))
        return FALSE;

    help_set_zoom(help_instance, delta);
    return TRUE;
}

static void
help_page_free(gpointer data)
{
    HelpPage *page = data;

    if (help_instance == page)
        help_instance = NULL;

    if (page->zoom_css != NULL)
    {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                      GTK_STYLE_PROVIDER(page->zoom_css));
        g_object_unref(page->zoom_css);
    }

    g_free(page->reference);
    g_free(page);
}

static void
help_add_row(HelpPage *page,
             gint index,
             const gchar *group,
             const gchar *title)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *title_label = gtk_label_new(title);
    GtkWidget *group_label = gtk_label_new(group);

    gtk_widget_add_css_class(title_label, "zg-cell-bold");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_widget_add_css_class(group_label, "zg-dim");
    gtk_label_set_xalign(GTK_LABEL(group_label), 0.0f);

    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_box_append(GTK_BOX(box), title_label);
    gtk_box_append(GTK_BOX(box), group_label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    g_object_set_data(G_OBJECT(row), "topic-index", GINT_TO_POINTER(index));
    gtk_list_box_append(GTK_LIST_BOX(page->list), row);

    (void)page;
}

GtkWidget *
zg_page_help_new(ZgApp *app)
{
    HelpPage *page = g_new0(HelpPage, 1);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *intro;

    GtkWidget *paned;
    GtkWidget *list_scrolled;
    GtkWidget *text_scrolled;
    gchar *reference_title;
    guint i;

    page->app = app;
    page->zoom = 100;
    page->zoom_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(page->zoom_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);

    intro = zg_ui_label_wrap(
            "这里汇总了每个页面的用法、危险操作的注意事项与全部受控操作说明。"
            "在左侧选择主题，或在搜索框输入关键词（如“快照”“发送”“密码”）。"
            "字号可用 Ctrl+加号 / Ctrl+减号 缩放，Ctrl+0 复位。");
    gtk_widget_add_css_class(intro, "zg-dim");
    gtk_box_append(GTK_BOX(root), intro);

    page->search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(page->search,
                                          "搜索功能、操作名称、操作 ID 或说明…");
    gtk_widget_set_hexpand(GTK_WIDGET(page->search), FALSE);
    g_signal_connect(page->search, "search-changed",
                     G_CALLBACK(on_help_search_changed), page);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(page->search));

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 300);

    page->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(page->list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(page->list, "zg-help-list");
    g_signal_connect(page->list, "row-selected",
                     G_CALLBACK(on_help_row_selected), page);

    for (i = 0; i < G_N_ELEMENTS(topics); i++)
        help_add_row(page, (gint)i, topics[i].group, topics[i].title);

    page->reference = build_operations_reference();
    reference_title = g_strdup_printf("完整操作参考（%u 项）", zg_op_count());
    help_add_row(page, HELP_REFERENCE_INDEX, "参考", reference_title);
    g_free(reference_title);

    list_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scrolled), page->list);
    gtk_widget_set_size_request(list_scrolled, 280, -1);
    gtk_paned_set_start_child(GTK_PANED(paned), list_scrolled);

    text_scrolled = zg_ui_text_view_new(&page->buffer, TRUE);
    gtk_widget_add_css_class(text_scrolled, "zg-help-text");
    gtk_widget_set_hexpand(text_scrolled, TRUE);
    gtk_widget_set_vexpand(text_scrolled, TRUE);
    gtk_widget_set_margin_start(text_scrolled, 10);
    gtk_paned_set_end_child(GTK_PANED(paned), text_scrolled);

    gtk_box_append(GTK_BOX(root), paned);

    help_apply_zoom(page);
    help_instance = page;

    g_object_set_data_full(G_OBJECT(root), "zg-help", page, help_page_free);

    gtk_list_box_select_row(GTK_LIST_BOX(page->list),
                            GTK_LIST_BOX_ROW(gtk_widget_get_first_child(page->list)));

    return root;
}
