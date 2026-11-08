/* zg-util.h - 通用字符串 / 数值 / 名称校验工具
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_UTIL_H
#define ZG_UTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* 字符串 */
gboolean zg_str_is_empty(const char *str);
const char *zg_str_or(const char *str,
                      const char *fallback);
gchar *zg_str_trim(const char *str);
gchar *zg_str_join(GPtrArray *items,
                   const char *separator);

/* 数值与时间格式化（ZFS 风格，二进制单位） */
gchar *zg_format_bytes(guint64 bytes);
gchar *zg_format_epoch_local(gint64 epoch);
gchar *zg_format_epoch_relative(gint64 epoch);
gboolean zg_parse_u64(const char *str,
                      guint64 *value_out);

/* 名称合法性校验：所有以 '-' 开头的字符串一律拒绝，
 * 以避免把用户输入当作命令行选项传入特权进程。 */
gboolean zg_pool_name_valid(const char *name);
gboolean zg_dataset_name_valid(const char *name);
gboolean zg_snapshot_name_valid(const char *name);
gboolean zg_vdev_spec_valid(const char *spec);
gboolean zg_simple_name_valid(const char *name);
gboolean zg_user_name_valid(const char *name);

G_END_DECLS

#endif /* ZG_UTIL_H */
