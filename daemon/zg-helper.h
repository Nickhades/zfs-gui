/* zg-helper.h - 与特权 Helper 通信（能力模式下的 mount/unmount） */

#pragma once

#include <glib.h>

#include "zg-argv.h"
#include "zg-ops.h"

G_BEGIN_DECLS

gboolean zg_helper_supports(const gchar *op_id);
ZgOpOutcome *zg_helper_call(const ZgOpDef *op,
                            const ZgOpRequest *request);

G_END_DECLS
