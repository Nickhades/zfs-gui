/* zg-libzfs-exec.h - 守护进程的可选 libzfs 执行层（阶段 B1）
 *
 * 仅在环境变量 ZFS_GUI_LIBZFS_EXEC=1 时启用；未实现/不支持的参数组合
 * 返回 FALSE，由调用方回退到原有 execv 路径。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#pragma once

#include <glib.h>

#include "zg-argv.h"
#include "zg-ops.h"

G_BEGIN_DECLS

/* 返回 TRUE 表示已处理（outcome 必非 NULL）；FALSE 表示回退 CLI */
gboolean zg_libzfs_exec_try(const ZgOpDef *op,
                            const ZgOpRequest *request,
                            ZgOpOutcome **outcome);

/* 能力模式准备：打开并限制 /dev/zfs、注入 libzfs、预热 ioctl。
 * 必须在 cap_enter() 之前调用；成功后子进程可继承已初始化的 handle。 */
gboolean zg_libzfs_exec_capsicum_prepare(void);

/* 能力模式下允许的操作（不需要额外文件/设备路径访问） */
gboolean zg_libzfs_exec_capsicum_supports(const gchar *op_id);

G_END_DECLS
