/* zg-page-pools.c - 存储池页面（对象直连操作）
 *
 * 交互：左侧点击「存储池 / vdev / 磁盘」→ 顶部操作面板立即列出该对象
 * 可执行的语义化操作，点一下就能完成，无需填写任何命令参数。
 *
 * 键盘：Enter 打开属性/状态，F5 刷新，Delete 销毁（仅池）。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <string.h>

#include "zg-actions.h"
#include "zg-app.h"
#include "zg-client.h"
#include "zg-dialogs.h"
#include "zg-model.h"
#include "zg-pages.h"
#include "zg-table.h"
#include "zg-ui.h"
#include "zg-util.h"

enum
{
    POOL_COL_NAME,
    POOL_COL_STATE,
    POOL_COL_INFO,
    POOL_COL_EXTRA,
    POOL_COL_KIND, /* pool / vdev / disk */
    POOL_COL_POOLNAME,
    POOL_COL_VDEVNAME,
    POOL_COL_PATH,
    POOL_COL_DEPTH,
    POOL_N_COLUMNS
};

enum
{
    PROP_COL_NAME,
    PROP_COL_VALUE,
    PROP_COL_SOURCE,
    PROP_N_COLUMNS
};

enum
{
    POOL_SEL_NONE = 0,
    POOL_SEL_POOL,
    POOL_SEL_VDEV,
    POOL_SEL_DISK
};

enum
{
    PAGE_OVERVIEW = 0,
    PAGE_PROPERTIES,
    PAGE_STATUS,
    PAGE_HISTORY,
    PAGE_IOSTAT
};

typedef struct
{
    ZgApp *app;
    GtkWidget *root;
    GtkWidget *panel;
    GListStore *store;
    GtkWidget *table;
    GListStore *props_store;
    GtkWidget *props_table;
    GtkWidget *notebook;

    GtkWidget *ov_size;
    GtkWidget *ov_alloc;
    GtkWidget *ov_free;
    GtkWidget *ov_cap;
    GtkWidget *ov_health;
    GtkWidget *ov_frag;
    GtkWidget *ov_dedup;
    GtkWidget *ov_altroot;
    GtkWidget *ov_scan;

    GtkTextBuffer *status_buffer;
    GtkTextBuffer *history_buffer;
    GtkTextBuffer *iostat_buffer;

    GPtrArray *pools;
    GPtrArray *statuses;

    gchar *selected;
    gchar *sel_pool;
    gchar *sel_vdev;
    gchar *sel_path;
    gint sel_kind;
} PoolsPage;

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                   */
/* ------------------------------------------------------------------------- */

static void
set_string(gchar **slot,
           const gchar *value)
{
    g_free(*slot);
    *slot = g_strdup(value);
}

static GtkWindow *
page_window(PoolsPage *page)
{
    return GTK_WINDOW(gtk_widget_get_root(page->root));
}

static const gchar *
selected_device(PoolsPage *page)
{
    if (page->sel_path != NULL && page->sel_path[0] != '\0')
        return page->sel_path;

    return page->sel_vdev;
}

static GPtrArray *
page_used_devices(PoolsPage *page)
{
    GPtrArray *used = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; page->statuses != NULL && i < page->statuses->len; i++)
    {
        ZgPoolStatus *status = g_ptr_array_index(page->statuses, i);

        if (status->root != NULL)
        {
            GQueue *queue = g_queue_new();

            g_queue_push_tail(queue, status->root);

            while (!g_queue_is_empty(queue))
            {
                ZgVdev *vdev = g_queue_pop_head(queue);
                guint j;

                if (vdev->name != NULL)
                    g_ptr_array_add(used, g_strdup(vdev->name));

                if (vdev->path != NULL)
                    g_ptr_array_add(used, g_strdup(vdev->path));

                for (j = 0; j < vdev->children->len; j++)
                    g_queue_push_tail(queue, g_ptr_array_index(vdev->children, j));
            }

            g_queue_free(queue);
        }
    }

    g_ptr_array_add(used, NULL);

    return used;
}

/* ------------------------------------------------------------------------- */
/* 完成回调                                                                   */
/* ------------------------------------------------------------------------- */

static void
on_action_done(gboolean success,
               gpointer user_data)
{
    (void)success;
    (void)user_data;
}

/* ------------------------------------------------------------------------- */
/* 选择相关的操作                                                             */
/* ------------------------------------------------------------------------- */

static void
on_act_create_pool(GtkButton *button,
                   gpointer user_data)
{
    (void)button;

    zg_dialogs_create_pool(page_window(user_data), on_action_done, user_data);
}

static void
on_act_import(GtkButton *button,
              gpointer user_data)
{
    (void)button;

    zg_dialogs_import_pool(page_window(user_data), on_action_done, user_data);
}

static void
on_act_refresh(GtkButton *button,
               gpointer user_data)
{
    (void)button;

    zg_app_refresh_all(zg_app_get_default());
}

static void
on_act_properties(GtkButton *button,
                  gpointer user_data)
{
    PoolsPage *page = user_data;

    (void)button;

    gtk_notebook_set_current_page(GTK_NOTEBOOK(page->notebook), PAGE_PROPERTIES);
}

static void
on_act_status(GtkButton *button,
              gpointer user_data)
{
    PoolsPage *page = user_data;

    (void)button;

    gtk_notebook_set_current_page(GTK_NOTEBOOK(page->notebook), PAGE_STATUS);
}

/* ---- 池级操作 ---- */

typedef struct
{
    PoolsPage *page;
    gchar *op_id;
    gchar *message;
} PoolActionContext;

static void
pool_action_context_free(gpointer data)
{
    PoolActionContext *context = data;

    if (context == NULL)
        return;

    g_free(context->op_id);
    g_free(context->message);
    g_free(context);
}

static void
on_pool_choice_done(const gboolean *selected,
                    gpointer data)
{
    PoolActionContext *context = data;
    ZgArgs *args = zg_args_new();

    zg_args_add_positional(args, context->page->sel_pool);

    if (selected != NULL && selected[0])
        zg_args_add_flag(args, "-d");

    zg_dialogs_run_op(page_window(context->page), context->op_id, args,
                      context->message, on_action_done, context->page);

    pool_action_context_free(context);
}

static void
pool_maintenance(PoolsPage *page,
                 const gchar *op_id,
                 const gchar *title,
                 const gchar *heading,
                 const gchar *description,
                 const gchar *option_label,
                 const gchar *accept_label,
                 gboolean destructive,
                 const gchar *success_message)
{
    PoolActionContext *context = g_new0(PoolActionContext, 1);
    ZgChoice choice;

    context->page = page;
    context->op_id = g_strdup(op_id);
    context->message = g_strdup(success_message);

    choice.label = option_label;
    choice.hint = NULL;
    choice.active = FALSE;

    zg_dialogs_choice(page_window(page),
                      &(ZgChoiceSpec){.title = title,
                                      .heading = heading,
                                      .description = description,
                                      .choices = option_label != NULL ? &choice : NULL,
                                      .n_choices = option_label != NULL ? 1 : 0,
                                      .accept_label = accept_label,
                                      .destructive = destructive},
                      on_pool_choice_done, context);
}

static void
on_act_scrub(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.scrub", "校验存储池",
                     "开始 scrub 校验",
                     "scrub 会读取池中所有数据并校验校验和，发现可修复错误时会自动"
                     "使用冗余副本修复。过程可能持续很久，可以随时停止。",
                     "等待校验完成（界面会阻塞到结束）",
                     "开始校验", FALSE,
                     "校验已开始，可在「状态详情」中查看进度。");
}

static void
on_act_trim(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.trim", "TRIM 存储池",
                     "让 SSD 回收空闲块",
                     "TRIM 可改善 SSD 的写入性能；对机械硬盘没有意义。",
                     "持续 TRIM，直到手动停止",
                     "开始 TRIM", FALSE, "TRIM 已开始。");
}

static void
on_act_clear(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.clear", "清除错误计数",
                     "清除池的错误状态",
                     "如果故障磁盘已更换或问题已修复，可以清除错误计数让状态恢复。",
                     "同时丢弃失败的事务",
                     "清除错误", FALSE, "错误计数已清除。");
}

static void
on_act_upgrade(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.upgrade", "升级池版本",
                     "升级存储池的磁盘格式",
                     "升级后可以启用新版本特性，但旧版 FreeBSD 将无法再导入该池。",
                     NULL, "升级", FALSE, "存储池版本已升级。");
}

static void
on_act_initialize(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.initialize", "初始化空闲块",
                     "把池中的空闲块写入初始值",
                     "用于彻底清除设备上残留的历史数据，耗时较长。",
                     NULL, "开始初始化", FALSE, "初始化已在后台开始。");
}

static void
on_act_resilver(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.resilver", "重建冗余",
                     "对池中全部冗余 vdev 执行 resilver",
                     "重新校验并修复冗余数据，通常在更换磁盘后使用。",
                     NULL, "开始重建", FALSE, "重建已开始。");
}

static void
on_act_checkpoint(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.checkpoint", "创建检查点",
                     "为池创建一个检查点",
                     "检查点记录当前事务状态，可在必要时丢弃最近的数据变更。",
                     NULL, "创建检查点", FALSE, "检查点已创建。");
}

static void
on_act_discard_checkpoint(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.checkpoint", "丢弃检查点",
                     "丢弃池的检查点",
                     "丢弃后无法再回退到检查点之前的状态，请确认。",
                     NULL, "丢弃检查点", TRUE, "检查点已丢弃。");
}

static void
on_act_prefetch(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.prefetch", "预取元数据",
                     "把池元数据预取进 ARC",
                     "可改善首次访问的响应速度。",
                     NULL, "预取", FALSE, "元数据已预取。");
}

static void
on_act_sync(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.sync", "同步数据",
                     "把所有未写数据刷入磁盘",
                     "用于关机或迁移前确保数据落盘。",
                     NULL, "同步", FALSE, "数据已同步。");
}

static void
on_act_reopen(GtkButton *button, gpointer user_data)
{
    (void)button;

    pool_maintenance(user_data, "zpool.reopen", "重新打开设备",
                     "让池重新识别设备容量",
                     "扩容设备后使用，使 ZFS 感知新的容量。",
                     NULL, "重新打开", FALSE, "设备已重新打开。");
}

static void
on_act_export(GtkButton *button, gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgArgs *args = zg_args_new();
    gchar *pool = g_strdup(page->sel_pool);
    gchar *message;

    (void)button;

    zg_args_add_positional(args, pool);

    message = g_strdup_printf("存储池 %s 已导出，可在其他系统上导入。", pool);

    zg_dialogs_run_op(page_window(page), "zpool.export", args, message,
                      on_action_done, page);

    g_free(message);
    g_free(pool);
}

static void
on_act_destroy(GtkButton *button, gpointer user_data)
{
    PoolsPage *page = user_data;
    gchar *pool = g_strdup(page->sel_pool);

    (void)button;

    zg_dialogs_destroy_target(page_window(page), pool, TRUE,
                              on_action_done, page);
    g_free(pool);
}

static void
on_act_add_disk(GtkButton *button, gpointer user_data)
{
    PoolsPage *page = user_data;
    GPtrArray *used = page_used_devices(page);

    (void)button;

    zg_dialogs_add_vdev(page_window(page), page->sel_pool,
                        (const gchar *const *)used->pdata,
                        on_action_done, page);

    g_ptr_array_free(used, TRUE);
}

/* ---- 设备级操作 ---- */

typedef struct
{
    PoolsPage *page;
    gchar *op_id;
    gchar *message;
} DeviceContext;

static void
device_context_free(gpointer data)
{
    DeviceContext *context = data;

    if (context == NULL)
        return;

    g_free(context->op_id);
    g_free(context->message);
    g_free(context);
}

static void
on_device_choice_done(const gboolean *selected,
                      gpointer data)
{
    DeviceContext *context = data;
    ZgArgs *args = zg_args_new();
    const gchar *device = selected_device(context->page);

    zg_args_add_positional(args, context->page->sel_pool);
    zg_args_add_positional(args, device);

    if (selected != NULL && selected[0])
        zg_args_add_flag(args, "-n");

    zg_dialogs_run_op(page_window(context->page), context->op_id, args,
                      context->message, on_action_done, context->page);

    device_context_free(context);
}

static void
device_action(PoolsPage *page,
              const gchar *op_id,
              const gchar *title,
              const gchar *heading,
              const gchar *description,
              const gchar *option_label,
              const gchar *accept_label,
              gboolean destructive,
              const gchar *success_message)
{
    DeviceContext *context = g_new0(DeviceContext, 1);
    ZgChoice choice;

    context->page = page;
    context->op_id = g_strdup(op_id);
    context->message = g_strdup(success_message);

    choice.label = option_label;
    choice.hint = NULL;
    choice.active = FALSE;

    zg_dialogs_choice(page_window(page),
                      &(ZgChoiceSpec){.title = title,
                                      .heading = heading,
                                      .description = description,
                                      .choices = option_label != NULL ? &choice : NULL,
                                      .n_choices = option_label != NULL ? 1 : 0,
                                      .accept_label = accept_label,
                                      .destructive = destructive},
                      on_device_choice_done, context);
}

static void
on_act_offline(GtkButton *button, gpointer user_data)
{
    (void)button;

    device_action(user_data, "zpool.offline", "离线设备",
                  "把设备标记为离线",
                  "离线后设备不再参与读写，适用于安全拔盘；镜像/RAID-Z 中其他成员"
                  "仍可继续提供服务。",
                  "仅演练，不实际修改",
                  "离线设备", FALSE, "设备已离线。");
}

static void
on_act_online(GtkButton *button, gpointer user_data)
{
    (void)button;

    device_action(user_data, "zpool.online", "上线设备",
                  "把设备重新加入池",
                  "让之前离线或故障排除后的设备重新参与读写。",
                  "仅演练，不实际修改",
                  "上线设备", FALSE, "设备已上线。");
}

static void
on_act_detach(GtkButton *button, gpointer user_data)
{
    (void)button;

    device_action(user_data, "zpool.detach", "拆除设备",
                  "从镜像中拆除这块盘",
                  "仅适用于镜像 vdev。拆除后冗余会降低，请先确认其他设备健康。",
                  NULL, "拆除设备", TRUE, "设备已从镜像中拆除。");
}

static void
on_act_remove_vdev(GtkButton *button, gpointer user_data)
{
    (void)button;

    device_action(user_data, "zpool.remove", "移除 vdev",
                  "从池中移除这个 vdev",
                  "若 vdev 中仍有数据，ZFS 会先迁移数据，可能需要较长时间。",
                  "仅演练，不实际移除",
                  "移除 vdev", TRUE, "vdev 已移除。");
}

static void
on_act_clear_device(GtkButton *button, gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgArgs *args = zg_args_new();
    const gchar *device = selected_device(page);
    gchar *message;

    (void)button;

    zg_args_add_positional(args, page->sel_pool);
    zg_args_add_positional(args, device);

    message = g_strdup_printf("已清除设备 %s 的错误计数。", device);

    zg_dialogs_run_op(page_window(page), "zpool.clear", args, message,
                      on_action_done, page);

    g_free(message);
}

static void
on_act_replace_disk(GtkButton *button, gpointer user_data)
{
    PoolsPage *page = user_data;
    GPtrArray *used = page_used_devices(page);
    const gchar *device = selected_device(page);

    (void)button;

    zg_dialogs_replace_disk(page_window(page), page->sel_pool, device,
                            (const gchar *const *)used->pdata,
                            on_action_done, page);

    g_ptr_array_free(used, TRUE);
}

/* ---- 维护菜单 ---- */

static void
on_act_maintenance(GtkButton *button,
                   gpointer user_data)
{
    PoolsPage *page = user_data;
    GtkWindow *window;
    GtkWidget *body;
    GtkWidget *close_row;
    GtkWidget *close_button;
    static const struct
    {
        const gchar *label;
        GCallback callback;
    } items[] = {
            {"升级池版本", G_CALLBACK(on_act_upgrade)},
            {"初始化空闲块", G_CALLBACK(on_act_initialize)},
            {"重建冗余", G_CALLBACK(on_act_resilver)},
            {"创建检查点", G_CALLBACK(on_act_checkpoint)},
            {"丢弃检查点", G_CALLBACK(on_act_discard_checkpoint)},
            {"预取元数据", G_CALLBACK(on_act_prefetch)},
            {"同步数据", G_CALLBACK(on_act_sync)},
            {"重新打开设备", G_CALLBACK(on_act_reopen)},
    };
    guint i;

    (void)button;

    window = zg_dialogs_window_new(page_window(page), "存储池维护", 560, 560);
    body = zg_dialogs_body(window, NULL);

    gtk_box_append(GTK_BOX(body), zg_ui_label_heading("维护操作"));
    gtk_box_append(GTK_BOX(body), zg_ui_label_dim(
                                          "这些操作会修改存储池状态，执行前会再次确认。"));

    for (i = 0; i < G_N_ELEMENTS(items); i++)
    {
        GtkWidget *item = zg_ui_button(items[i].label, items[i].callback, page);

        gtk_widget_set_halign(item, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(body), item);
    }

    close_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(close_row, GTK_ALIGN_END);
    close_button = gtk_button_new_with_label("关闭");
    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_close), window);
    gtk_box_append(GTK_BOX(close_row), close_button);
    gtk_box_append(GTK_BOX(body), close_row);

    gtk_window_present(window);
}

/* ------------------------------------------------------------------------- */
/* 操作面板                                                                   */
/* ------------------------------------------------------------------------- */

static gchar *
pool_sizes_text(PoolsPage *page)
{
    ZgPool *pool = zg_model_pool_find(page->pools, page->sel_pool);
    gchar *size;
    gchar *alloc;
    gchar *free_space;
    gchar *text;

    if (pool == NULL)
        return g_strdup("容量信息暂不可用");

    size = zg_format_bytes(pool->size);
    alloc = zg_format_bytes(pool->alloc);
    free_space = zg_format_bytes(pool->free);

    text = g_strdup_printf("总容量 %s · 已用 %s · 可用 %s · 使用率 %d%%",
                           size, alloc, free_space, pool->capacity);

    g_free(size);
    g_free(alloc);
    g_free(free_space);

    return text;
}

static void
pools_apply_usage(PoolsPage *page)
{
    ZgPool *pool = zg_model_pool_find(page->pools, page->sel_pool);
    gchar *used;
    gchar *total;
    gchar *label;

    if (pool == NULL || pool->size == 0)
    {
        zg_action_panel_set_usage(page->panel, -1.0, NULL);
        return;
    }

    used = zg_format_bytes(pool->alloc);
    total = zg_format_bytes(pool->size);
    label = g_strdup_printf("已用 %s / %s（%d%%）", used, total, pool->capacity);

    zg_action_panel_set_usage(page->panel,
                              (gdouble)pool->alloc / (gdouble)pool->size,
                              label);

    g_free(used);
    g_free(total);
    g_free(label);
}

static void
on_act_system_snapshot(GtkButton *button,
                       gpointer user_data)
{
    PoolsPage *page = user_data;
    GDateTime *now;
    gchar *stamp;
    gchar *snapshot;
    gchar *message;
    ZgArgs *args;

    (void)button;

    if (page->sel_pool == NULL)
    {
        zg_dialogs_error(page_window(page), "没有选择存储池",
                         "请先在上方列表中选择一个存储池。");
        return;
    }

    /* 自动命名：system-YYYYMMDD-HHMMSS，递归覆盖池内全部数据集 */
    now = g_date_time_new_now_local();
    stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    snapshot = g_strdup_printf("%s@system-%s", page->sel_pool, stamp);
    message = g_strdup_printf("系统快照已创建：%s（递归包含全部数据集）", snapshot);

    args = zg_args_new();
    zg_args_add_flag(args, "-r");
    zg_args_add_positional(args, snapshot);

    zg_dialogs_run_op(page_window(page), "zfs.snapshot", args, message, NULL, NULL);

    g_free(message);
    g_free(snapshot);
    g_free(stamp);
    g_date_time_unref(now);
}

static void
pools_update_panel(PoolsPage *page)
{
    const gchar *hint =
            "点击左侧对象查看操作 · Enter 打开属性 · F5 刷新 · Delete 销毁池";

    if (page->sel_kind == POOL_SEL_NONE)
    {
        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "drive-harddisk-symbolic",
                                           .title = "存储池",
                                           .subtitle = "还没有选择对象",
                                           .state = NULL,
                                           .actions = NULL,
                                           .n_actions = 0,
                                           .hint = "提示：在左侧点一个对象，再用上方按钮操作；刷新与命令中心见上方工具栏"});
        return;
    }

    if (page->sel_kind == POOL_SEL_POOL)
    {
        ZgPool *pool = zg_model_pool_find(page->pools, page->sel_pool);
        gchar *subtitle = pool_sizes_text(page);
        ZgAction actions[] = {
                {"属性", "document-properties-symbolic", "查看与修改池属性（Enter）",
                 FALSE, TRUE, FALSE, G_CALLBACK(on_act_properties), page},
                {"状态详情", "dialog-information-symbolic", "查看 vdev 与错误详情",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_status), page},
                {"添加磁盘", "list-add-symbolic", "把新磁盘加入这个池",
                 FALSE, FALSE, TRUE, G_CALLBACK(on_act_add_disk), page},
                {"校验 scrub", "security-high-symbolic", "校验并修复池中数据",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_scrub), page},
                {"TRIM", "user-trash-symbolic", "回收 SSD 空闲块",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_trim), page},
                {"清除错误", "edit-clear-symbolic", "清除错误计数",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_clear), page},
                {"维护", "emblem-system-symbolic", "升级 / 初始化 / 检查点等",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_maintenance), page},
                {"导出", "document-save-symbolic", "从系统导出，便于迁移",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_export), page},
                {"销毁", "edit-delete-symbolic", "永久删除整个池",
                 TRUE, FALSE, TRUE, G_CALLBACK(on_act_destroy), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "drive-harddisk-symbolic",
                                           .title = page->sel_pool,
                                           .subtitle = subtitle,
                                           .state = pool != NULL ? pool->health : NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = hint});
        pools_apply_usage(page);
        g_free(subtitle);
        return;
    }

    if (page->sel_kind == POOL_SEL_DISK)
    {
        const gchar *device = selected_device(page);
        ZgAction actions[] = {
                {"更换磁盘", "edit-copy-symbolic", "用新磁盘替换这块盘",
                 TRUE, TRUE, TRUE, G_CALLBACK(on_act_replace_disk), page},
                {"离线", "media-playback-stop-symbolic", "安全拔盘前先离线",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_offline), page},
                {"上线", "media-playback-start-symbolic", "把设备重新加入",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_online), page},
                {"拆除", "edit-delete-symbolic", "从镜像中移除这块盘",
                 TRUE, FALSE, FALSE, G_CALLBACK(on_act_detach), page},
                {"清除错误", "edit-clear-symbolic", "清除该设备的错误计数",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_clear_device), page},
                {"状态详情", "dialog-information-symbolic", "查看池的详细状态",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_status), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "drive-harddisk-symbolic",
                                           .title = page->selected,
                                           .subtitle = device != NULL ? device : page->sel_pool,
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = "磁盘操作直接影响数据安全 · 危险操作需要管理员密码"});
        pools_apply_usage(page);
        return;
    }

    {
        ZgAction actions[] = {
                {"移除 vdev", "edit-delete-symbolic", "从池中移除这个 vdev",
                 TRUE, TRUE, TRUE, G_CALLBACK(on_act_remove_vdev), page},
                {"状态详情", "dialog-information-symbolic", "查看池的详细状态",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_status), page},
                {"属性", "document-properties-symbolic", "查看池属性",
                 FALSE, FALSE, FALSE, G_CALLBACK(on_act_properties), page},
        };

        zg_action_panel_set(page->panel,
                            &(ZgPanelSpec){.icon = "view-list-symbolic",
                                           .title = page->selected,
                                           .subtitle = page->sel_pool,
                                           .state = NULL,
                                           .actions = actions,
                                           .n_actions = G_N_ELEMENTS(actions),
                                           .hint = hint});
        pools_apply_usage(page);
    }
}

/* ------------------------------------------------------------------------- */
/* 树构建                                                                     */
/* ------------------------------------------------------------------------- */
static void
append_vdev(PoolsPage *page,
            ZgRow *parent,
            ZgVdev *vdev,
            const gchar *pool_name)
{
    ZgRow *row;
    gchar *info;
    gchar *extra;
    gchar *label;
    const gchar *kind;
    gboolean is_disk;
    guint i;

    if (vdev == NULL)
        return;

    info = g_strdup_printf("%s / %s",
                           zg_str_or(vdev->alloc_space, "—"),
                           zg_str_or(vdev->total_space, "—"));

    extra = g_strdup_printf("读 %" G_GUINT64_FORMAT " · 写 %" G_GUINT64_FORMAT
                            " · 校验 %" G_GUINT64_FORMAT,
                            vdev->read_errors, vdev->write_errors,
                            vdev->checksum_errors);

    if (vdev->vdev_class != NULL &&
        g_strcmp0(vdev->vdev_class, "normal") != 0)
        label = g_strdup_printf("%s  [%s]", vdev->name, vdev->vdev_class);
    else
        label = g_strdup(vdev->name != NULL ? vdev->name : "?");

    is_disk = (g_strcmp0(vdev->type, "disk") == 0 ||
               g_strcmp0(vdev->type, "file") == 0);
    kind = is_disk ? "disk" : "vdev";

    row = zg_row_new();
    zg_row_set_text(row, POOL_COL_NAME, label);
    zg_row_set_text(row, POOL_COL_STATE, vdev->state != NULL ? vdev->state : "—");
    zg_row_set_text(row, POOL_COL_INFO, info);
    zg_row_set_text(row, POOL_COL_EXTRA, extra);
    zg_row_set_text(row, POOL_COL_KIND, kind);
    zg_row_set_text(row, POOL_COL_POOLNAME, pool_name);
    zg_row_set_text(row, POOL_COL_VDEVNAME, vdev->name);
    zg_row_set_text(row, POOL_COL_PATH, vdev->path);

    zg_row_add_child(parent, row);

    for (i = 0; i < vdev->children->len; i++)
        append_vdev(page, row, g_ptr_array_index(vdev->children, i), pool_name);

    g_object_unref(row);

    g_free(info);
    g_free(extra);
    g_free(label);
}
static void
pools_rebuild(PoolsPage *page)
{
    guint i;

    g_list_store_remove_all(page->store);

    for (i = 0; i < (page->statuses != NULL ? page->statuses->len : 0); i++)
    {
        ZgPoolStatus *status = g_ptr_array_index(page->statuses, i);
        ZgPool *summary = zg_model_pool_find(page->pools, status->name);
        ZgRow *row = zg_row_new();
        gchar *info;
        gchar *extra;

        if (summary != NULL)
        {
            gchar *size = zg_format_bytes(summary->size);
            gchar *alloc = zg_format_bytes(summary->alloc);
            gchar *free_space = zg_format_bytes(summary->free);

            info = g_strdup_printf("总 %s · 已用 %s · 可用 %s · %d%%",
                                   size, alloc, free_space, summary->capacity);
            extra = g_strdup_printf("碎片 %d%% · 去重 %.2fx",
                                    summary->fragmentation, summary->dedup_ratio);

            g_free(size);
            g_free(alloc);
            g_free(free_space);
        }
        else
        {
            info = g_strdup("—");
            extra = g_strdup("");
        }

        zg_row_set_text(row, POOL_COL_NAME, status->name);
        zg_row_set_text(row, POOL_COL_STATE, zg_str_or(status->state, "—"));
        zg_row_set_text(row, POOL_COL_INFO, info);
        zg_row_set_text(row, POOL_COL_EXTRA, extra);
        zg_row_set_text(row, POOL_COL_KIND, "pool");
        zg_row_set_text(row, POOL_COL_POOLNAME, status->name);
        zg_row_set_text(row, POOL_COL_VDEVNAME, status->name);
        zg_row_set_text(row, POOL_COL_PATH, "");

        if (status->root != NULL)
            append_vdev(page, row, status->root, status->name);

        g_list_store_append(page->store, row);
        g_object_unref(row);

        g_free(info);
        g_free(extra);
    }

    if ((page->statuses == NULL || page->statuses->len == 0) &&
        page->pools != NULL)
    {
        for (i = 0; i < page->pools->len; i++)
        {
            ZgPool *pool = g_ptr_array_index(page->pools, i);
            ZgRow *row = zg_row_new();
            gchar *size = zg_format_bytes(pool->size);
            gchar *alloc = zg_format_bytes(pool->alloc);
            gchar *free_space = zg_format_bytes(pool->free);
            gchar *info = g_strdup_printf("总 %s · 已用 %s · 可用 %s · %d%%",
                                          size, alloc, free_space, pool->capacity);

            zg_row_set_text(row, POOL_COL_NAME, pool->name);
            zg_row_set_text(row, POOL_COL_STATE, zg_str_or(pool->health, "—"));
            zg_row_set_text(row, POOL_COL_INFO, info);
            zg_row_set_text(row, POOL_COL_EXTRA, "");
            zg_row_set_text(row, POOL_COL_KIND, "pool");
            zg_row_set_text(row, POOL_COL_POOLNAME, pool->name);
            zg_row_set_text(row, POOL_COL_VDEVNAME, pool->name);
            zg_row_set_text(row, POOL_COL_PATH, "");

            g_list_store_append(page->store, row);
            g_object_unref(row);

            g_free(size);
            g_free(alloc);
            g_free(free_space);
            g_free(info);
        }
    }

    zg_table_select_first(page->table);
}

/* ------------------------------------------------------------------------- */
/* 详情加载                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    PoolsPage *page;
    gchar *pool;
} DetailContext;

static void
detail_context_free(gpointer data)
{
    DetailContext *context = data;

    if (context == NULL)
        return;

    g_free(context->pool);
    g_free(context);
}

static gboolean
detail_is_current(DetailContext *context)
{
    return context->page->sel_pool != NULL &&
           g_strcmp0(context->page->sel_pool, context->pool) == 0;
}

static void
on_status_detail(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DetailContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (detail_is_current(context))
    {
        if (response != NULL && response->ok)
        {
            const gchar *scan = strstr(response->stdout_text, "scan:");

            zg_ui_text_set(context->page->status_buffer, response->stdout_text);

            if (scan != NULL)
            {
                const gchar *end = strchr(scan, '\n');
                gchar *line = (end != NULL)
                                      ? g_strndup(scan + 5, (gsize)(end - scan - 5))
                                      : g_strdup(scan + 5);
                gchar *trimmed = zg_str_trim(line);

                gtk_label_set_text(GTK_LABEL(context->page->ov_scan), trimmed);
                g_free(trimmed);
                g_free(line);
            }
        }
        else
        {
            zg_ui_text_set(context->page->status_buffer,
                           response != NULL ? response->message : "读取失败");
        }
    }

    zg_client_result_free(response);
    detail_context_free(context);
}

static void
on_history_detail(GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
    DetailContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (detail_is_current(context))
        zg_ui_text_set(context->page->history_buffer,
                       (response != NULL && response->ok)
                               ? response->stdout_text
                               : (response != NULL ? response->message : "读取失败"));

    zg_client_result_free(response);
    detail_context_free(context);
}

static void
on_iostat_detail(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    DetailContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (detail_is_current(context))
        zg_ui_text_set(context->page->iostat_buffer,
                       (response != NULL && response->ok)
                               ? (response->stdout_text[0] != '\0' ? response->stdout_text
                                                                   : "（无 I/O 统计输出）")
                               : (response != NULL ? response->message : "读取失败"));

    zg_client_result_free(response);
    detail_context_free(context);
}

static void
on_props_detail(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
    DetailContext *context = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GPtrArray *properties;
    guint i;

    (void)source;

    if (detail_is_current(context))
    {
        g_list_store_remove_all(context->page->props_store);

        properties = zg_model_properties_from_tsv(
                (response != NULL && response->ok) ? response->stdout_text : "");

        for (i = 0; i < properties->len; i++)
        {
            ZgProperty *property = g_ptr_array_index(properties, i);
            ZgRow *row = zg_row_new();

            zg_row_set_text(row, PROP_COL_NAME, property->property);
            zg_row_set_text(row, PROP_COL_VALUE, property->value);
            zg_row_set_text(row, PROP_COL_SOURCE, property->source);

            g_list_store_append(context->page->props_store, row);
            g_object_unref(row);
        }

        g_ptr_array_free(properties, TRUE);
    }

    zg_client_result_free(response);
    detail_context_free(context);
}

static void
pools_update_overview(PoolsPage *page,
                      const gchar *pool_name)
{
    ZgPool *pool = zg_model_pool_find(page->pools, pool_name);
    ZgPoolStatus *status = zg_model_status_find(page->statuses, pool_name);
    gchar *text;

    if (pool == NULL)
        return;

    text = zg_format_bytes(pool->size);
    gtk_label_set_text(GTK_LABEL(page->ov_size), text);
    g_free(text);

    text = zg_format_bytes(pool->alloc);
    gtk_label_set_text(GTK_LABEL(page->ov_alloc), text);
    g_free(text);

    text = zg_format_bytes(pool->free);
    gtk_label_set_text(GTK_LABEL(page->ov_free), text);
    g_free(text);

    text = g_strdup_printf("%d%%", pool->capacity);
    gtk_label_set_text(GTK_LABEL(page->ov_cap), text);
    g_free(text);

    zg_ui_state_label_set(page->ov_health,
                          zg_str_or(pool->health,
                                    status != NULL ? status->state : "—"));

    if (pool->fragmentation >= 0)
    {
        text = g_strdup_printf("%d%%", pool->fragmentation);
        gtk_label_set_text(GTK_LABEL(page->ov_frag), text);
        g_free(text);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(page->ov_frag), "—");
    }

    text = g_strdup_printf("%.2fx", pool->dedup_ratio);
    gtk_label_set_text(GTK_LABEL(page->ov_dedup), text);
    g_free(text);

    gtk_label_set_text(GTK_LABEL(page->ov_altroot),
                       zg_str_or(pool->altroot, "—"));
    gtk_label_set_text(GTK_LABEL(page->ov_scan),
                       (status != NULL && status->scan != NULL)
                               ? status->scan
                               : "—");
}

static void
pools_load_detail(PoolsPage *page,
                  const gchar *pool)
{
    DetailContext *context;
    ZgArgs *args;

    if (pool == NULL)
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-v");
    zg_args_add_positional(args, pool);
    context = g_new0(DetailContext, 1);
    context->page = page;
    context->pool = g_strdup(pool);
    zg_client_call_async("zpool.status", zg_args_to_json(args),
                         on_status_detail, context);

    args = zg_args_new();
    zg_args_add_positional(args, pool);
    context = g_new0(DetailContext, 1);
    context->page = page;
    context->pool = g_strdup(pool);
    zg_client_call_async("zpool.history", zg_args_to_json(args),
                         on_history_detail, context);

    args = zg_args_new();
    zg_args_add_flag(args, "-v");
    zg_args_add_flag(args, "-y");
    zg_args_add_positional(args, pool);
    context = g_new0(DetailContext, 1);
    context->page = page;
    context->pool = g_strdup(pool);
    zg_client_call_async("zpool.iostat", zg_args_to_json(args),
                         on_iostat_detail, context);

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_args_add_positional(args, "all");
    zg_args_add_positional(args, pool);
    context = g_new0(DetailContext, 1);
    context->page = page;
    context->pool = g_strdup(pool);
    zg_client_call_async("zpool.get", zg_args_to_json(args),
                         on_props_detail, context);
}

/* ------------------------------------------------------------------------- */
/* 选择与键盘                                                                 */
/* ------------------------------------------------------------------------- */
static void
on_selection_changed(GtkSelectionModel *selection,
                     guint position,
                     guint n_items,
                     gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgRow *row;
    const gchar *name;
    const gchar *kind;
    const gchar *pool;
    const gchar *vdev;
    const gchar *path;

    (void)selection;
    (void)position;
    (void)n_items;

    row = zg_table_get_selected(page->table);

    if (row == NULL)
    {
        page->sel_kind = POOL_SEL_NONE;
        set_string(&page->selected, NULL);
        pools_update_panel(page);
        return;
    }

    name = zg_row_get_text(row, POOL_COL_NAME);
    kind = zg_row_get_text(row, POOL_COL_KIND);
    pool = zg_row_get_text(row, POOL_COL_POOLNAME);
    vdev = zg_row_get_text(row, POOL_COL_VDEVNAME);
    path = zg_row_get_text(row, POOL_COL_PATH);

    set_string(&page->selected, name);
    set_string(&page->sel_pool, pool);
    set_string(&page->sel_vdev, vdev);
    set_string(&page->sel_path, path);

    if (g_strcmp0(kind, "pool") == 0)
        page->sel_kind = POOL_SEL_POOL;
    else if (g_strcmp0(kind, "disk") == 0)
        page->sel_kind = POOL_SEL_DISK;
    else
        page->sel_kind = POOL_SEL_VDEV;

    pools_update_panel(page);
    pools_update_overview(page, page->sel_pool);
    pools_load_detail(page, page->sel_pool);

    if (page->sel_kind == POOL_SEL_DISK)
        zg_app_set_status(page->app, "磁盘 %s（池 %s）", page->selected, page->sel_pool);
    else if (page->sel_kind == POOL_SEL_VDEV)
        zg_app_set_status(page->app, "vdev %s（池 %s）", page->selected, page->sel_pool);
    else
        zg_app_set_status(page->app, "存储池 %s", page->sel_pool);
}

static void
on_table_activated(GtkColumnView *view,
                   guint position,
                   gpointer user_data)
{
    (void)view;
    (void)position;

    on_act_properties(NULL, user_data);
}

static gboolean
on_tree_key_pressed(GtkEventControllerKey *controller,
                    guint keyval,
                    guint keycode,
                    GdkModifierType state,
                    gpointer user_data)
{
    PoolsPage *page = user_data;

    (void)controller;
    (void)keycode;
    (void)state;

    switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        if (page->sel_kind == POOL_SEL_POOL)
            on_act_properties(NULL, page);
        else if (page->sel_kind != POOL_SEL_NONE)
            on_act_status(NULL, page);
        return TRUE;
    case GDK_KEY_F5:
        zg_app_refresh_all(page->app);
        return TRUE;
    case GDK_KEY_Delete:
        if (page->sel_kind == POOL_SEL_POOL)
            on_act_destroy(NULL, page);
        return TRUE;
    default:
        return FALSE;
    }
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

static void
on_pools_list(GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
    {
        if (page->pools != NULL)
            g_ptr_array_free(page->pools, TRUE);

        page->pools = zg_model_pools_from_tsv(response->stdout_text);
        pools_rebuild(page);
    }
    else
    {
        zg_app_set_status(page->app, "读取池列表失败：%s",
                          response != NULL ? response->message : "无应答");
    }

    zg_client_result_free(response);
}

static void
on_pools_status_json(GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
    {
        if (page->statuses != NULL)
            g_ptr_array_free(page->statuses, TRUE);

        page->statuses = zg_model_statuses_from_json(response->stdout_text, NULL);
        pools_rebuild(page);
    }

    zg_client_result_free(response);
}

static void
on_pools_status_text(GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);

    (void)source;

    if (response != NULL && response->ok)
    {
        if (page->statuses != NULL)
            g_ptr_array_free(page->statuses, TRUE);

        page->statuses = zg_model_statuses_from_text(response->stdout_text);
        pools_rebuild(page);
    }

    zg_client_result_free(response);
}

static void
pools_refresh(gpointer data)
{
    PoolsPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");
    zg_client_call_async("zpool.list", zg_args_to_json(args),
                         on_pools_list, page);

    if (zg_app_json_output(page->app))
    {
        args = zg_args_new();
        zg_args_add_flag(args, "-j");
        zg_client_call_async("zpool.status", zg_args_to_json(args),
                             on_pools_status_json, page);
    }
    else
    {
        args = zg_args_new();
        zg_client_call_async("zpool.status", zg_args_to_json(args),
                             on_pools_status_text, page);
    }
}

/* ------------------------------------------------------------------------- */
/* 属性编辑                                                                   */
/* ------------------------------------------------------------------------- */

static void
on_edit_property(GtkButton *button,
                 gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgRow *row;
    const gchar *property;
    const gchar *value;
    const gchar *source;

    (void)button;

    if (page->sel_pool == NULL)
        return;

    row = zg_table_get_selected(page->props_table);

    if (row == NULL)
    {
        zg_dialogs_error(page_window(page), "未选择属性",
                         "请先在属性列表中选中一行。");
        return;
    }

    property = zg_row_get_text(row, PROP_COL_NAME);
    value = zg_row_get_text(row, PROP_COL_VALUE);
    source = zg_row_get_text(row, PROP_COL_SOURCE);

    zg_dialogs_property_editor(page_window(page), page->sel_pool, TRUE,
                               property, value, source);
}

static void
on_props_row_activated(GtkColumnView *view,
                       guint position,
                       gpointer user_data)
{
    (void)view;
    (void)position;

    on_edit_property(NULL, user_data);
}

/* ------------------------------------------------------------------------- */
/* 界面构建                                                                   */
/* ------------------------------------------------------------------------- */

static GtkWidget *
build_overview_tab(PoolsPage *page)
{
    GtkWidget *scrolled = gtk_scrolled_window_new();
    GtkWidget *grid = zg_ui_grid_new();
    gint row = 0;

    page->ov_size = zg_ui_grid_add(grid, &row, "总容量", "—");
    page->ov_alloc = zg_ui_grid_add(grid, &row, "已分配", "—");
    page->ov_free = zg_ui_grid_add(grid, &row, "可分配", "—");
    page->ov_cap = zg_ui_grid_add(grid, &row, "使用率", "—");
    page->ov_health = zg_ui_grid_add(grid, &row, "健康状态", "—");
    page->ov_frag = zg_ui_grid_add(grid, &row, "碎片率", "—");
    page->ov_dedup = zg_ui_grid_add(grid, &row, "去重比", "—");
    page->ov_altroot = zg_ui_grid_add(grid, &row, "altroot", "—");
    page->ov_scan = zg_ui_grid_add(grid, &row, "最近扫描", "—");

    gtk_label_set_wrap(GTK_LABEL(page->ov_scan), TRUE);

    {
        GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_widget_add_css_class(card, "zg-card");
        gtk_widget_set_margin_top(card, 12);
        gtk_widget_set_margin_bottom(card, 12);
        gtk_widget_set_margin_start(card, 12);
        gtk_widget_set_margin_end(card, 12);
        gtk_box_append(GTK_BOX(card), grid);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), card);
    }

    return scrolled;
}

static GtkWidget *
build_props_tab(PoolsPage *page)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scrolled = gtk_scrolled_window_new();

    page->props_store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"属性", PROP_COL_NAME, FALSE, 18, 0.0f, TRUE, FALSE},
                {"值", PROP_COL_VALUE, TRUE, 24, 0.0f, FALSE, FALSE},
                {"来源", PROP_COL_SOURCE, FALSE, 10, 0.0f, FALSE, TRUE},
        };

        page->props_table = zg_table_new_flat(page->props_store, columns,
                                              G_N_ELEMENTS(columns));
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->props_table);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_widget_set_margin_top(scrolled, 10);
    gtk_widget_set_margin_start(scrolled, 10);
    gtk_widget_set_margin_end(scrolled, 10);
    gtk_box_append(GTK_BOX(box), scrolled);

    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_widget_set_margin_top(row, 4);
        gtk_widget_set_margin_bottom(row, 4);
        gtk_widget_set_halign(row, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(row),
                       zg_ui_button("编辑属性", G_CALLBACK(on_edit_property), page));
        gtk_box_append(GTK_BOX(box), row);
    }

    g_signal_connect(page->props_table, "activate",
                     G_CALLBACK(on_props_row_activated), page);

    return box;
}

/* ------------------------------------------------------------------------- */
/* 实时容量刷新（只更新行文本，不重建表格）                                   */
/* ------------------------------------------------------------------------- */

static void
on_pools_live(GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
    PoolsPage *page = user_data;
    ZgClientResult *response = zg_client_call_finish(result);
    GListModel *model;
    guint i;
    guint n;

    (void)source;

    if (response == NULL || !response->ok)
    {
        zg_client_result_free(response);
        return;
    }

    if (page->pools != NULL)
        g_ptr_array_free(page->pools, TRUE);

    page->pools = zg_model_pools_from_tsv(response->stdout_text);

    model = G_LIST_MODEL(page->store);
    n = g_list_model_get_n_items(model);

    for (i = 0; i < n; i++)
    {
        ZgRow *row = ZG_ROW(g_list_model_get_item(model, i));
        const gchar *kind = zg_row_get_text(row, POOL_COL_KIND);
        const gchar *name = zg_row_get_text(row, POOL_COL_POOLNAME);

        if (g_strcmp0(kind, "pool") == 0)
        {
            ZgPool *pool = zg_model_pool_find(page->pools, name);

            if (pool != NULL)
            {
                gchar *size = zg_format_bytes(pool->size);
                gchar *alloc = zg_format_bytes(pool->alloc);
                gchar *free_space = zg_format_bytes(pool->free);
                gchar *info = g_strdup_printf("总 %s · 已用 %s · 可用 %s · %d%%",
                                              size, alloc, free_space,
                                              pool->capacity);
                gchar *extra = g_strdup_printf("碎片 %d%% · 去重 %.2fx",
                                               pool->fragmentation,
                                               pool->dedup_ratio);

                zg_row_set_text(row, POOL_COL_INFO, info);
                zg_row_set_text(row, POOL_COL_EXTRA, extra);
                zg_row_set_text(row, POOL_COL_STATE,
                                pool->health != NULL ? pool->health : "—");

                g_free(size);
                g_free(alloc);
                g_free(free_space);
                g_free(info);
                g_free(extra);
            }
        }

        g_object_unref(row);
    }

    if (page->sel_pool != NULL)
        pools_update_overview(page, page->sel_pool);

    pools_apply_usage(page);

    zg_client_result_free(response);
}

static void
pools_live_refresh(gpointer data)
{
    PoolsPage *page = data;
    ZgArgs *args;

    if (!zg_app_is_connected(page->app))
        return;

    args = zg_args_new();
    zg_args_add_flag(args, "-H");
    zg_args_add_flag(args, "-p");

    zg_client_call_async("zpool.list", zg_args_to_json(args), on_pools_live, page);
}

GtkWidget *
zg_page_pools_new(ZgApp *app)
{
    PoolsPage *page = g_new0(PoolsPage, 1);
    GtkWidget *paned;
    GtkWidget *left;
    GtkWidget *toolbar;
    GtkWidget *scrolled;
    GtkWidget *notebook;
    GtkWidget *status_text;
    GtkWidget *history_text;
    GtkWidget *iostat_text;
    GtkSelectionModel *selection;
    GtkEventController *keys;

    page->app = app;

    page->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* 对象操作面板 */
    page->panel = zg_action_panel_new();
    gtk_box_append(GTK_BOX(page->root), page->panel);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(page->root), paned);

    left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    /* 全局动作：创建 / 导入 */
    toolbar = zg_ui_toolbar_new();
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("创建存储池", G_CALLBACK(on_act_create_pool), page));
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("导入存储池", G_CALLBACK(on_act_import), page));
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("刷新", G_CALLBACK(on_act_refresh), page));
    zg_ui_toolbar_add(toolbar,
                      zg_ui_button("系统快照", G_CALLBACK(on_act_system_snapshot), page));
    gtk_box_append(GTK_BOX(left), toolbar);

    page->store = g_list_store_new(ZG_TYPE_ROW);

    {
        static const ZgTableColumn columns[] = {
                {"池 / 设备", POOL_COL_NAME, TRUE, 18, 0.0f, TRUE, FALSE},
                {"状态", POOL_COL_STATE, FALSE, 8, 0.0f, FALSE, FALSE},
                {"容量 / 已用", POOL_COL_INFO, TRUE, 20, 0.0f, FALSE, FALSE},
                {"附加信息", POOL_COL_EXTRA, TRUE, 22, 0.0f, FALSE, TRUE},
        };

        page->table = zg_table_new_tree(page->store, columns,
                                        G_N_ELEMENTS(columns));
    }

    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page->table);
    /* 列较多时允许横向滚动，避免目录树被裁切 */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled), FALSE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(left), scrolled);

    selection = zg_table_get_selection(page->table);
    g_signal_connect(selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), page);
    g_signal_connect(page->table, "activate", G_CALLBACK(on_table_activated), page);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_tree_key_pressed), page);
    gtk_widget_add_controller(page->table, keys);

    notebook = gtk_notebook_new();
    page->notebook = notebook;
    gtk_paned_set_end_child(GTK_PANED(paned), notebook);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_overview_tab(page),
                             gtk_label_new("概况"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_props_tab(page),
                             gtk_label_new("属性"));

    status_text = zg_ui_text_view_new(&page->status_buffer, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), status_text,
                             gtk_label_new("状态详情"));

    history_text = zg_ui_text_view_new(&page->history_buffer, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), history_text,
                             gtk_label_new("历史"));

    iostat_text = zg_ui_text_view_new(&page->iostat_buffer, FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), iostat_text,
                             gtk_label_new("I/O 统计"));

    gtk_paned_set_position(GTK_PANED(paned), 820);

    g_object_set_data_full(G_OBJECT(page->root), "zg-page-pools", page, g_free);

    zg_app_register_refresh(app, pools_refresh, page);
    zg_app_register_live_refresh(app, pools_live_refresh, page);
    pools_update_panel(page);

    return page->root;
}
