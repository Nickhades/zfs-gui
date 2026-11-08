/* zg-ops.c - 受控操作执行层实现
 *
 * 两类操作：
 *   1. 通用操作：注册表中的 tool + subcommand，参数经 zg-argv 校验后 execv。
 *   2. 内部操作（tool == NULL）：system.info / system.arcstats，
 *      在守护进程内直接实现，不接受任何用户参数。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-ops.h"

#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "zg-auth.h"
#include "zg-exec.h"
#include "zg-json.h"
#include "zg-log.h"
#include "zg-protocol.h"
#include "zg-util.h"

/* 守护进程版本，随 meson 的 project() 版本保持一致 */
#define ZG_DAEMON_VERSION "1.0.0"

static gint64
now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;

    return (gint64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static ZgOpOutcome *
outcome_new(void)
{
    ZgOpOutcome *outcome = g_new0(ZgOpOutcome, 1);

    return outcome;
}

void zg_op_outcome_free(ZgOpOutcome *outcome)
{
    if (outcome == NULL)
        return;

    g_free(outcome->stdout_text);
    g_free(outcome->stderr_text);
    g_free(outcome->data_json);
    g_free(outcome->message);
    g_free(outcome);
}

static void
set_failure(ZgOpOutcome *outcome,
            gint code,
            const gchar *message)
{
    outcome->ok = FALSE;
    outcome->code = code;
    g_free(outcome->message);
    outcome->message = g_strdup(message);
}

/* ------------------------------------------------------------------------- */
/* 能力探测                                                                   */
/* ------------------------------------------------------------------------- */

static gboolean
detect_json_output(void)
{
    static gint cached = -1;
    ZgExecResult *result;
    gchar *tool;
    gchar *argv[4];
    gboolean supported = FALSE;

    if (cached >= 0)
        return cached == 1;

    tool = zg_exec_resolve_tool("zpool");

    if (tool == NULL)
    {
        cached = 0;
        return FALSE;
    }

    argv[0] = tool;
    argv[1] = (gchar *)"list";
    argv[2] = (gchar *)"-j";
    argv[3] = NULL;

    result = zg_exec_run(argv, NULL, NULL, 15);

    if (result != NULL)
    {
        const gchar *text = result->stdout_text->str;

        while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')
            text++;

        supported = (result->exit_code == 0 && text[0] == '{');
        zg_exec_result_free(result);
    }

    g_free(tool);
    cached = supported ? 1 : 0;

    return supported;
}

static gchar *
read_sysctl_u64(const gchar *name)
{
    guint64 value = 0;
    gsize length = sizeof(value);

    if (sysctlbyname(name, &value, &length, NULL, 0) < 0)
        return NULL;

    return g_strdup_printf("%" G_GUINT64_FORMAT, value);
}

/* ------------------------------------------------------------------------- */
/* system.info                                                                */
/* ------------------------------------------------------------------------- */

static ZgOpOutcome *
system_info(const ZgPolicy *policy)
{
    ZgOpOutcome *outcome = outcome_new();
    ZgJsonWriter *writer;
    struct utsname info;
    gchar *data;
    gchar *zpool_path;
    gchar *zfs_path;
    gchar *bectl_path;
    gchar *userland = NULL;
    gchar *kernel = NULL;
    gchar *arc_size;
    gchar *arc_max;
    ZgExecResult *version;

    memset(&info, 0, sizeof(info));

    if (uname(&info) < 0)
    {
        memset(&info, 0, sizeof(info));
        g_strlcpy(info.sysname, "FreeBSD", sizeof(info.sysname));
    }

    zpool_path = zg_exec_resolve_tool("zpool");
    zfs_path = zg_exec_resolve_tool("zfs");
    bectl_path = zg_exec_resolve_tool("bectl");
    arc_size = read_sysctl_u64("kstat.zfs.misc.arcstats.size");
    arc_max = read_sysctl_u64("kstat.zfs.misc.arcstats.c_max");

    if (zfs_path != NULL)
    {
        gchar *argv[3];

        argv[0] = zfs_path;
        argv[1] = (gchar *)"version";
        argv[2] = NULL;

        version = zg_exec_run(argv, NULL, NULL, 15);

        if (version != NULL && version->exit_code == 0)
        {
            gchar **lines = g_strsplit(version->stdout_text->str, "\n", -1);
            guint i;

            for (i = 0; lines[i] != NULL; i++)
            {
                gchar *line = zg_str_trim(lines[i]);

                if (line[0] == '\0')
                {
                    g_free(line);
                    continue;
                }

                if (userland == NULL)
                    userland = g_strdup(line);
                else if (kernel == NULL)
                    kernel = g_strdup(line);

                g_free(line);
            }

            g_strfreev(lines);
        }

        if (version != NULL)
            zg_exec_result_free(version);
    }

    writer = zg_json_writer_new();
    zg_json_writer_begin_object(writer, NULL);
    zg_json_writer_set_int(writer, "protocol", ZG_PROTOCOL_VERSION);
    zg_json_writer_set_string(writer, "daemon_version", ZG_DAEMON_VERSION);
    zg_json_writer_set_string(writer, "hostname", info.nodename);
    zg_json_writer_set_string(writer, "arch", info.machine);
    zg_json_writer_set_string(writer, "os", info.sysname);
    zg_json_writer_set_string(writer, "osrelease", info.release);
    zg_json_writer_set_string(writer, "zfs_userland", userland);
    zg_json_writer_set_string(writer, "zfs_kernel", kernel);
    zg_json_writer_set_bool(writer, "json_output", detect_json_output());
    zg_json_writer_set_string(writer, "socket_path", zg_policy_socket_path(policy));
    zg_json_writer_set_string(writer, "auth_mode", zg_auth_mode(policy));
    zg_json_writer_set_int(writer, "euid", (gint64)geteuid());
    zg_json_writer_set_int(writer, "now", (gint64)time(NULL));
    zg_json_writer_set_string(writer, "arc_size", arc_size);
    zg_json_writer_set_string(writer, "arc_max", arc_max);

    zg_json_writer_begin_object(writer, "tools");
    zg_json_writer_set_string(writer, "zpool", zpool_path);
    zg_json_writer_set_string(writer, "zfs", zfs_path);
    zg_json_writer_set_string(writer, "bectl", bectl_path);
    zg_json_writer_end_object(writer);

    zg_json_writer_end_object(writer);

    data = zg_json_writer_to_string(writer, NULL);

    outcome->ok = TRUE;
    outcome->code = 0;
    outcome->data_json = data;
    outcome->stdout_text = g_strdup("");
    outcome->stderr_text = g_strdup("");
    outcome->message = g_strdup("系统信息读取成功");

    g_free(zpool_path);
    g_free(zfs_path);
    g_free(bectl_path);
    g_free(userland);
    g_free(kernel);
    g_free(arc_size);
    g_free(arc_max);

    return outcome;
}

/* ------------------------------------------------------------------------- */
/* system.arcstats                                                            */
/* ------------------------------------------------------------------------- */

static ZgOpOutcome *
system_arcstats(void)
{
    ZgOpOutcome *outcome = outcome_new();
    gchar *sysctl_path = zg_exec_resolve_tool("sysctl");
    ZgExecResult *result;

    if (sysctl_path == NULL)
    {
        set_failure(outcome, -2, "系统中未找到 sysctl 命令");
        return outcome;
    }

    {
        gchar *argv[3];

        argv[0] = sysctl_path;
        argv[1] = (gchar *)"kstat.zfs.misc.arcstats";
        argv[2] = NULL;

        result = zg_exec_run(argv, NULL, NULL, 30);
    }

    g_free(sysctl_path);

    if (result == NULL)
    {
        set_failure(outcome, -2, "执行 sysctl 失败");
        return outcome;
    }

    outcome->stdout_text = g_strdup(result->stdout_text->str);
    outcome->stderr_text = g_strdup(result->stderr_text->str);

    if (result->exit_code == 0)
    {
        outcome->ok = TRUE;
        outcome->code = 0;
        outcome->message = g_strdup("ARC 统计读取成功");
    }
    else
    {
        set_failure(outcome, result->exit_code,
                    result->stderr_text->len > 0
                            ? result->stderr_text->str
                            : "读取 ARC 统计失败（可能内核未加载 ZFS）");
    }

    zg_exec_result_free(result);

    return outcome;
}

/* ------------------------------------------------------------------------- */
/* 通用操作                                                                   */
/* ------------------------------------------------------------------------- */

static ZgOpOutcome *
run_tool(const ZgOpDef *op,
         const ZgOpRequest *request,
         const ZgPolicy *policy)
{
    ZgOpOutcome *outcome = outcome_new();
    gchar *tool_path;
    gchar **argv;
    ZgExecResult *result;
    guint timeout;
    const gchar *stdout_path = NULL;
    const gchar *stdin_path = NULL;

    if (op->tool == NULL)
    {
        set_failure(outcome, -2, "内部操作缺少实现");
        return outcome;
    }

    tool_path = zg_exec_resolve_tool(op->tool);

    if (tool_path == NULL)
    {
        gchar *message = g_strdup_printf(
                "系统中未找到 %s 命令（已搜索 /sbin、/usr/sbin、/usr/local/sbin 与 PATH）。\n"
                "请确认已安装 base 系统的 ZFS 工具，或安装 sysutils/openzfs。",
                op->tool);

        set_failure(outcome, -2, message);
        g_free(message);

        return outcome;
    }

    argv = zg_op_build_argv(op, request, tool_path);

    if (argv == NULL)
    {
        set_failure(outcome, -2, "参数组装失败");
        g_free(tool_path);
        return outcome;
    }

    timeout = op->long_running ? ZG_LONG_OP_TIMEOUT_SECONDS : policy->io_timeout;

    if (op->io == ZG_OP_IO_OUT_FILE)
        stdout_path = request->io_path;

    if (op->io == ZG_OP_IO_IN_FILE)
        stdin_path = request->io_path;

    if (stdin_path != NULL && !g_file_test(stdin_path, G_FILE_TEST_EXISTS))
    {
        set_failure(outcome, -2, "输入文件不存在");
        g_strfreev(argv);
        g_free(tool_path);
        return outcome;
    }

    result = zg_exec_run(argv, stdout_path, stdin_path, timeout);

    g_strfreev(argv);
    g_free(tool_path);

    if (result == NULL)
    {
        set_failure(outcome, -2, "创建子进程失败");
        return outcome;
    }

    outcome->stdout_text = g_strdup(result->stdout_text->str);
    outcome->stderr_text = g_strdup(result->stderr_text->str);
    outcome->code = result->exit_code;

    if (result->error_message != NULL)
    {
        set_failure(outcome, result->exit_code, result->error_message);
    }
    else if (result->timed_out)
    {
        set_failure(outcome, -3, "操作超时");
    }
    else if (result->exit_code != 0)
    {
        const gchar *detail = result->stderr_text->len > 0
                                      ? result->stderr_text->str
                                      : result->stdout_text->str;

        set_failure(outcome, result->exit_code,
                    detail[0] != '\0' ? detail : "命令执行失败");
    }
    else
    {
        outcome->ok = TRUE;
        outcome->message = g_strdup(op->io == ZG_OP_IO_OUT_FILE
                                            ? "命令执行成功，数据已写入目标文件"
                                            : "命令执行成功");
    }

    if (result->truncated)
        zg_log_warning("操作 %s 的输出被截断（超过 %u 字节）", op->id, ZG_MAX_OUTPUT);

    zg_exec_result_free(result);

    return outcome;
}

ZgOpOutcome *
zg_op_execute(const ZgOpDef *op,
              const ZgOpRequest *request,
              const ZgPolicy *policy)
{
    ZgOpOutcome *outcome;
    gint64 started;

    g_return_val_if_fail(op != NULL, NULL);

    started = now_ms();

    if (g_strcmp0(op->id, "system.info") == 0)
        outcome = system_info(policy);
    else if (g_strcmp0(op->id, "system.arcstats") == 0)
        outcome = system_arcstats();
    else
        outcome = run_tool(op, request, policy);

    if (outcome != NULL)
        outcome->duration_ms = now_ms() - started;

    return outcome;
}
