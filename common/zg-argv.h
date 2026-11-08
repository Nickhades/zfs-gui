/* zg-argv.h - 请求解析、白名单校验与 argv 组装
 *
 * 安全模型
 * --------
 * 前端只能提交结构化参数；本模块负责：
 *   1. 拒绝未在注册表中登记的开关/选项；
 *   2. 拒绝以 '-' 开头的“位置参数”，杜绝把用户输入当作选项注入；
 *   3. 按类型校验字符集（池名、数据集名、设备路径……）；
 *   4. 最终只生成 argv 数组交给 execv()，永不经过 shell。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_ARGV_H
#define ZG_ARGV_H

#include <glib.h>

#include "zg-json.h"
#include "zg-opdef.h"

G_BEGIN_DECLS

typedef struct
{
    char *key;
    char *value;
} ZgOpOption;

typedef struct
{
    GPtrArray *flags;      /* char *        */
    GPtrArray *options;    /* ZgOpOption *  */
    GPtrArray *positional; /* char *        */
    char *io_path;         /* 重定向文件路径，可为 NULL */
} ZgOpRequest;

ZgOpRequest *zg_op_request_new(void);
void zg_op_request_free(ZgOpRequest *request);

/* 从请求的 args 对象解析出结构化参数 */
gboolean zg_op_request_parse(const ZgJson *args,
                             ZgOpRequest *request,
                             GError **error);

/* 依据操作定义做完整白名单校验 */
gboolean zg_op_request_validate(const ZgOpDef *op,
                                const ZgOpRequest *request,
                                GError **error);

/* 生成以 NULL 结尾的 argv（调用方使用 g_strfreev 释放） */
char **zg_op_build_argv(const ZgOpDef *op,
                        const ZgOpRequest *request,
                        const char *tool_path);

/* 生成可读命令行，用于日志与界面展示 */
char *zg_op_describe(const ZgOpDef *op,
                     const ZgOpRequest *request);

/* 单值类型校验（前端可复用以做即时提示） */
gboolean zg_value_valid(ZgValueKind kind,
                        const char *value);

G_END_DECLS

#endif /* ZG_ARGV_H */
