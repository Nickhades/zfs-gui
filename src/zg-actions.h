/* zg-actions.h - 对象操作面板
 *
 * 交互模型：先在树/列表里「点对象」，再在面板里「点功能」。
 * 面板按当前选中的对象（池、vdev、磁盘、数据集、快照、启动环境）
 * 动态列出可用操作，每个操作都对应一个语义化的对话框，
 * 用户不需要接触任何命令行参数。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_ACTIONS_H
#define ZG_ACTIONS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct
{
    const gchar *label;      /* 中文操作名 */
    const gchar *icon;       /* 符号图标名，可为 NULL */
    const gchar *tooltip;    /* 提示（含快捷键） */
    gboolean danger;         /* 红色危险按钮 */
    gboolean primary;        /* 主操作，高亮 */
    gboolean needs_password; /* 需要管理员密码，显示锁标记 */
    GCallback callback;
    gpointer data;
} ZgAction;

GtkWidget *zg_action_panel_new(void);

/* 重建面板：对象标题 + 状态 + 操作按钮 */
/* 对象卡片的全部内容；字段为空时使用默认值 */
typedef struct
{
    const gchar *icon;     /* 符号图标名 */
    const gchar *title;    /* 对象名 */
    const gchar *subtitle; /* 容量 / 挂载点等 */
    const gchar *state;    /* 健康状态，可为 NULL */
    const ZgAction *actions;
    guint n_actions;
    const gchar *hint; /* 键盘提示 */
} ZgPanelSpec;

void zg_action_panel_set(GtkWidget *panel, const ZgPanelSpec *spec);

/* 容量进度条：fraction < 0 表示隐藏 */
void zg_action_panel_set_usage(GtkWidget *panel,
                               gdouble fraction,
                               const gchar *label);

G_END_DECLS

#endif /* ZG_ACTIONS_H */
