/* zg-json.h - 极简 JSON 解析/生成器（仅依赖 GLib）
 *
 * 为什么自己实现：前端需要解析 `zpool status -j` / `zfs list -j`
 * （OpenZFS >= 2.2）的结构化输出，同时要能生成协议请求。
 * 引入 json-glib 会额外增加部署依赖，而这里只需要只读 DOM 与流式写入。
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#ifndef ZG_JSON_H
#define ZG_JSON_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------------- */
/* 错误域                                                                     */
/* ------------------------------------------------------------------------- */

#define ZG_JSON_ERROR (zg_json_error_quark())
GQuark zg_json_error_quark(void);

typedef enum
{
    ZG_JSON_ERROR_PARSE,
    ZG_JSON_ERROR_RANGE,
    ZG_JSON_ERROR_USAGE,
} ZgJsonError;

/* ------------------------------------------------------------------------- */
/* 只读 DOM                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum
{
    ZG_JSON_TYPE_NULL,
    ZG_JSON_TYPE_BOOL,
    ZG_JSON_TYPE_NUMBER,
    ZG_JSON_TYPE_STRING,
    ZG_JSON_TYPE_ARRAY,
    ZG_JSON_TYPE_OBJECT,
} ZgJsonType;

typedef struct _ZgJson ZgJson;

/* 解析一段 JSON 文本；len < 0 时按 NUL 结尾处理。
 * 失败返回 NULL 并设置 error。调用方负责 zg_json_free()。 */
ZgJson *zg_json_parse(const char *text,
                      gssize len,
                      GError **error);
void zg_json_free(ZgJson *node);

ZgJsonType zg_json_get_type(const ZgJson *node);
gboolean zg_json_is_null(const ZgJson *node);

const char *zg_json_get_string(const ZgJson *node);
gboolean zg_json_get_bool(const ZgJson *node,
                          gboolean fallback);
gint64 zg_json_get_int(const ZgJson *node,
                       gint64 fallback);

/* 数组 */
guint zg_json_array_size(const ZgJson *node);
const ZgJson *zg_json_array_at(const ZgJson *node,
                               guint index);

/* 对象 */
const ZgJson *zg_json_object_get(const ZgJson *node,
                                 const char *key);
guint zg_json_object_size(const ZgJson *node);
const char *zg_json_object_key_at(const ZgJson *node,
                                  guint index);
const ZgJson *zg_json_object_value_at(const ZgJson *node,
                                      guint index);

/* 便捷访问器（对象成员） */
const char *zg_json_object_get_string(const ZgJson *node,
                                      const char *key);
const char *zg_json_object_get_string_or(const ZgJson *node,
                                         const char *key,
                                         const char *fallback);
gboolean zg_json_object_get_bool(const ZgJson *node,
                                 const char *key,
                                 gboolean fallback);
gint64 zg_json_object_get_int(const ZgJson *node,
                              const char *key,
                              gint64 fallback);

/* ------------------------------------------------------------------------- */
/* 写入器（流式生成）                                                          */
/* ------------------------------------------------------------------------- */

typedef struct _ZgJsonWriter ZgJsonWriter;

ZgJsonWriter *zg_json_writer_new(void);
void zg_json_writer_free(ZgJsonWriter *writer);

/* key 为 NULL 时用于顶层值或数组元素 */
void zg_json_writer_begin_object(ZgJsonWriter *writer,
                                 const char *key);
void zg_json_writer_end_object(ZgJsonWriter *writer);
void zg_json_writer_begin_array(ZgJsonWriter *writer,
                                const char *key);
void zg_json_writer_end_array(ZgJsonWriter *writer);

void zg_json_writer_set_string(ZgJsonWriter *writer,
                               const char *key,
                               const char *value);
void zg_json_writer_set_int(ZgJsonWriter *writer,
                            const char *key,
                            gint64 value);
void zg_json_writer_set_bool(ZgJsonWriter *writer,
                             const char *key,
                             gboolean value);
void zg_json_writer_set_null(ZgJsonWriter *writer,
                             const char *key);
/* 插入一段已经生成好的 JSON 文本（仅内部使用，调用方保证合法） */
void zg_json_writer_set_raw(ZgJsonWriter *writer,
                            const char *key,
                            const char *raw_json);

void zg_json_writer_add_string(ZgJsonWriter *writer,
                               const char *value);
void zg_json_writer_add_int(ZgJsonWriter *writer,
                            gint64 value);
void zg_json_writer_add_raw(ZgJsonWriter *writer,
                            const char *raw_json);

/* 结束写入并返回所属字符串，同时释放 writer */
char *zg_json_writer_to_string(ZgJsonWriter *writer,
                               gsize *length);

/* 把 str 作为 JSON 字符串字面量追加到 out（含首尾引号） */
void zg_json_escape(GString *out,
                    const char *str);

/* 把任意节点重新序列化为 JSON 文本（调用方 g_free） */
char *zg_json_to_text(const ZgJson *node);

G_END_DECLS

#endif /* ZG_JSON_H */
