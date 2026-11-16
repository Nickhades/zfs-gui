/* main.c - ZFS 图形管理工具入口
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <gtk/gtk.h>

#include "zg-app.h"

int main(int argc, char **argv)
{
    ZgApp *app;
    gint status;

    /* 固定浅色主题，避免跟随桌面深浅色导致配色不一致；
     * 用户显式设置 GTK_THEME 时尊重用户的选择。 */
    if (g_getenv("GTK_THEME") == NULL)
        g_setenv("GTK_THEME", "Adwaita", TRUE);

    app = zg_app_new();
    status = zg_app_run(app, argc, argv);

    return status;
}
