/* zfs-guid.c - ZFS-GUI 特权后台守护进程
 *
 * 职责：以 root 身份执行经过白名单校验的 ZFS 命令，供前端 zfs-gui 调用。
 *
 * 用法：
 *   zfs-guid [-f] [-c 配置文件] [-p pidfile] [--version]
 *     -f, --foreground  前台运行（调试用）
 *     -c, --config      配置文件路径
 *     -p, --pidfile     PID 文件路径
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "zg-log.h"
#include "zg-policy.h"
#include "zg-protocol.h"
#include "zg-server.h"

#define ZG_DAEMON_VERSION "1.0.0"

static gboolean opt_foreground = FALSE;
static gchar *opt_config = NULL;
static gchar *opt_pidfile = NULL;
static gboolean opt_version = FALSE;

static GOptionEntry option_entries[] = {
        {"foreground", 'f', 0, G_OPTION_ARG_NONE, &opt_foreground,
         "在前台运行并输出日志到 stderr", NULL},
        {"config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config,
         "配置文件路径（默认 " ZG_CONFIG_PATH_DEFAULT "）", "文件"},
        {"pidfile", 'p', 0, G_OPTION_ARG_FILENAME, &opt_pidfile,
         "PID 文件路径（默认 " ZG_PID_PATH_DEFAULT "）", "文件"},
        {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version,
         "显示版本号", NULL},
        {.long_name = NULL}};

static gboolean
write_pidfile(const gchar *path)
{
    gchar *text;
    int fd;

    if (path == NULL || path[0] == '\0')
        return TRUE;

    text = g_strdup_printf("%ld\n", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
    {
        zg_log_warning("无法写入 PID 文件 %s：%s", path, g_strerror(errno));
        g_free(text);
        return FALSE;
    }

    if (write(fd, text, strlen(text)) < 0)
        zg_log_warning("写入 PID 文件失败：%s", g_strerror(errno));

    close(fd);
    g_free(text);

    return TRUE;
}

int main(int argc, char **argv)
{
    GOptionContext *context;
    GError *error = NULL;
    ZgPolicy *policy;
    const gchar *pidfile;
    int status;

    context = g_option_context_new("- ZFS 图形管理工具的特权后台服务");
    g_option_context_add_main_entries(context, option_entries, NULL);
    g_option_context_set_summary(context,
                                 "zfs-guid 以 root 身份运行，仅接受经过白名单校验的 ZFS 操作请求。\n"
                                 "前端通过 " ZG_SOCKET_PATH_DEFAULT " 与之通信。");

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_printerr("参数错误：%s\n", error->message);
        g_clear_error(&error);
        g_option_context_free(context);
        return 1;
    }

    g_option_context_free(context);

    if (opt_version)
    {
        g_print("zfs-guid %s（协议版本 %d）\n", ZG_DAEMON_VERSION, ZG_PROTOCOL_VERSION);
        return 0;
    }

    if (geteuid() != 0)
    {
        /* ZFS_GUI_ALLOW_NONROOT 仅用于开发调试：非 root 运行时特权操作必然失败，
         * 不会带来权限提升风险，但绝不应在生产环境设置。 */
        if (g_getenv("ZFS_GUI_ALLOW_NONROOT") == NULL)
        {
            g_printerr("[错误] zfs-guid 必须以 root 身份运行，当前 euid 为 %u。\n"
                       "如需开发调试，可设置 ZFS_GUI_ALLOW_NONROOT=1 后重试。\n",
                       (guint)geteuid());
            return 1;
        }

        g_printerr("[警告] ZFS_GUI_ALLOW_NONROOT 已设置：当前以非 root 身份运行，"
                   "绝大多数 ZFS 操作会失败（仅用于界面调试）。\n");
    }

    umask(0077);

    if (!opt_foreground)
    {
        if (daemon(0, 0) < 0)
        {
            g_printerr("[错误] 转入后台失败：%s\n", g_strerror(errno));
            return 1;
        }
    }

    zg_log_open(opt_foreground, "zfs-guid");

    policy = zg_policy_load(opt_config, &error);

    if (policy == NULL)
    {
        zg_log_error("读取配置失败：%s", error != NULL ? error->message : "未知错误");
        g_clear_error(&error);
        zg_log_close();
        return 1;
    }

    pidfile = (opt_pidfile != NULL) ? opt_pidfile : ZG_PID_PATH_DEFAULT;
    write_pidfile(pidfile);

    zg_log_info("zfs-guid %s 启动（协议 v%d）", ZG_DAEMON_VERSION, ZG_PROTOCOL_VERSION);

    status = zg_server_run(policy);

    unlink(pidfile);
    zg_policy_free(policy);
    zg_log_close();

    g_free(opt_config);
    g_free(opt_pidfile);

    return status == 0 ? 0 : 1;
}
