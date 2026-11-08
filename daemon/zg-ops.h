/* zg-ops.h - 受控操作执行层
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_OPS_H
#define ZG_OPS_H

#include <glib.h>

#include "zg-argv.h"
#include "zg-opdef.h"
#include "zg-policy.h"

G_BEGIN_DECLS

typedef struct
{
    gboolean ok;
    gint code; /* 子进程退出码，负数表示前端/策略层错误 */
    gchar *stdout_text;
    gchar *stderr_text;
    gchar *data_json; /* 可选的结构化应答 */
    gchar *message;   /* 面向用户的说明 */
    gint64 duration_ms;
} ZgOpOutcome;

ZgOpOutcome *zg_op_execute(const ZgOpDef *op,
                           const ZgOpRequest *request,
                           const ZgPolicy *policy);
void zg_op_outcome_free(ZgOpOutcome *outcome);

G_END_DECLS

#endif /* ZG_OPS_H */
