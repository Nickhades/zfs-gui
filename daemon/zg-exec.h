/* zg-exec.h - 子进程执行封装（fork/execv，不经过 shell）
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_EXEC_H
#define ZG_EXEC_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
    GString *stdout_text;
    GString *stderr_text;
    gint exit_code;   /* -1 表示未能取得退出码 */
    gint term_signal; /* 被信号终止时的信号编号 */
    gboolean timed_out;
    gboolean truncated;   /* 输出超过上限被截断 */
    gchar *error_message; /* 启动阶段失败信息 */
} ZgExecResult;

/* argv[0] 必须是绝对路径。
 * stdout_path 非空时把子进程 stdout 重定向到该文件；
 * stdin_path  非空时把子进程 stdin  重定向自该文件。
 * timeout_seconds 为 0 表示不设超时。 */
ZgExecResult *zg_exec_run(char *const argv[],
                          const gchar *stdout_path,
                          const gchar *stdin_path,
                          guint timeout_seconds);

void zg_exec_result_free(ZgExecResult *result);

/* 解析系统工具路径：/sbin、/usr/local/sbin 等候选目录 + PATH */
gchar *zg_exec_resolve_tool(const gchar *name);

G_END_DECLS

#endif /* ZG_EXEC_H */
