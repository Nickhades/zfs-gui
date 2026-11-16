/* zg-helper.h - 与特权 Helper 通信（能力模式下的 mount/unmount） */

#pragma once

#include <glib.h>

#include "zg-argv.h"
#include "zg-ops.h"

G_BEGIN_DECLS

#define ZG_HELPER_POOL_SIZE 5

gboolean zg_helper_supports(const gchar *op_id);

/* cap_enter 前预连接 Helper，并限制 fd 权利 */
gboolean zg_helper_pool_open(guint count,
                             GError **error);
void zg_helper_pool_close(void);

/* 子进程选择自己独占的预连接槽位 */
void zg_helper_set_slot(gint slot);
ZgOpOutcome *zg_helper_call(const ZgOpDef *op,
                            const ZgOpRequest *request);

/* 通过 SCM_RIGHTS 把已打开的文件 fd 交给 Helper 执行 zfs send */
ZgOpOutcome *zg_helper_send_stream(const gchar *snapshot,
                                   gint file_fd);

G_END_DECLS
