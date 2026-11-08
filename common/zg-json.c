/* zg-json.c - 极简 JSON 解析/生成器实现
 *
 * 版权所有 (c) 2026, BSD-2-Clause
 */

#include "zg-json.h"

#include <stdarg.h>
#include <string.h>

#define ZG_JSON_MAX_DEPTH 200

/* ------------------------------------------------------------------------- */
/* 错误域                                                                     */
/* ------------------------------------------------------------------------- */

GQuark
zg_json_error_quark(void)
{
    static GQuark quark = 0;

    if (quark == 0)
        quark = g_quark_from_static_string("zg-json-error");

    return quark;
}

/* ------------------------------------------------------------------------- */
/* DOM 结构                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
    char *key;
    ZgJson *value;
} ZgJsonMember;

struct _ZgJson
{
    ZgJsonType type;
    union
    {
        gboolean boolean;
        gdouble number;
        char *string;
        GPtrArray *array;  /* ZgJson*        */
        GPtrArray *object; /* ZgJsonMember*  */
    } u;
};

static ZgJson *
zg_json_new(ZgJsonType type)
{
    ZgJson *node = g_new0(ZgJson, 1);

    node->type = type;

    return node;
}

void zg_json_free(ZgJson *node)
{
    guint i;

    if (node == NULL)
        return;

    switch (node->type)
    {
    case ZG_JSON_TYPE_STRING:
        g_free(node->u.string);
        break;
    case ZG_JSON_TYPE_ARRAY:
        if (node->u.array != NULL)
        {
            for (i = 0; i < node->u.array->len; i++)
                zg_json_free(g_ptr_array_index(node->u.array, i));
            g_ptr_array_free(node->u.array, TRUE);
        }
        break;
    case ZG_JSON_TYPE_OBJECT:
        if (node->u.object != NULL)
        {
            for (i = 0; i < node->u.object->len; i++)
            {
                ZgJsonMember *member = g_ptr_array_index(node->u.object, i);
                g_free(member->key);
                zg_json_free(member->value);
                g_free(member);
            }
            g_ptr_array_free(node->u.object, TRUE);
        }
        break;
    default:
        break;
    }

    g_free(node);
}

/* ------------------------------------------------------------------------- */
/* 解析器                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct
{
    const char *data;
    gsize length;
    gsize pos;
    guint depth;
    GError **error;
    gboolean failed;
} ZgJsonParser;

static ZgJson *parse_value(ZgJsonParser *parser);

static void G_GNUC_PRINTF(2, 3)
        parse_fail(ZgJsonParser *parser,
                   const char *format,
                   ...)
{
    va_list args;
    char *message;

    if (parser->failed)
        return;

    parser->failed = TRUE;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    g_set_error(parser->error,
                ZG_JSON_ERROR,
                ZG_JSON_ERROR_PARSE,
                "JSON 解析失败（偏移 %" G_GSIZE_FORMAT "）：%s",
                parser->pos,
                message);
    g_free(message);
}

static void
skip_whitespace(ZgJsonParser *parser)
{
    while (parser->pos < parser->length)
    {
        const char c = parser->data[parser->pos];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            parser->pos++;
        else
            break;
    }
}

static gboolean
expect_char(ZgJsonParser *parser,
            char expected)
{
    skip_whitespace(parser);

    if (parser->pos >= parser->length)
    {
        parse_fail(parser, "意外的输入结束，期望 '%c'", expected);
        return FALSE;
    }

    if (parser->data[parser->pos] != expected)
    {
        parse_fail(parser, "期望 '%c'，实际为 '%c'",
                   expected, parser->data[parser->pos]);
        return FALSE;
    }

    parser->pos++;

    return TRUE;
}

static gint
hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static void
append_codepoint(GString *out,
                 guint32 codepoint)
{
    if (codepoint >= 0xd800 && codepoint <= 0xdfff)
        codepoint = 0xfffd; /* 孤立代理项，替换为 U+FFFD */

    if (codepoint < 0x80)
    {
        g_string_append_c(out, (char)codepoint);
    }
    else if (codepoint < 0x800)
    {
        g_string_append_c(out, (char)(0xc0 | (codepoint >> 6)));
        g_string_append_c(out, (char)(0x80 | (codepoint & 0x3f)));
    }
    else if (codepoint < 0x10000)
    {
        g_string_append_c(out, (char)(0xe0 | (codepoint >> 12)));
        g_string_append_c(out, (char)(0x80 | ((codepoint >> 6) & 0x3f)));
        g_string_append_c(out, (char)(0x80 | (codepoint & 0x3f)));
    }
    else
    {
        g_string_append_c(out, (char)(0xf0 | (codepoint >> 18)));
        g_string_append_c(out, (char)(0x80 | ((codepoint >> 12) & 0x3f)));
        g_string_append_c(out, (char)(0x80 | ((codepoint >> 6) & 0x3f)));
        g_string_append_c(out, (char)(0x80 | (codepoint & 0x3f)));
    }
}

static gboolean
parse_hex4(ZgJsonParser *parser,
           guint32 *value_out)
{
    guint32 value = 0;
    int i;

    if (parser->pos + 4 > parser->length)
    {
        parse_fail(parser, "\\u 转义不完整");
        return FALSE;
    }

    for (i = 0; i < 4; i++)
    {
        const gint digit = hex_digit_value(parser->data[parser->pos + i]);

        if (digit < 0)
        {
            parse_fail(parser, "\\u 转义包含非法十六进制字符");
            return FALSE;
        }

        value = (value << 4) | (guint32)digit;
    }

    parser->pos += 4;
    *value_out = value;

    return TRUE;
}

static char *
parse_string_literal(ZgJsonParser *parser)
{
    GString *out;

    skip_whitespace(parser);

    if (parser->pos >= parser->length || parser->data[parser->pos] != '"')
    {
        parse_fail(parser, "期望字符串");
        return NULL;
    }

    parser->pos++;
    out = g_string_new(NULL);

    while (parser->pos < parser->length)
    {
        const char c = parser->data[parser->pos];

        if (c == '"')
        {
            parser->pos++;
            return g_string_free(out, FALSE);
        }

        if (c == '\\')
        {
            parser->pos++;

            if (parser->pos >= parser->length)
            {
                parse_fail(parser, "字符串转义不完整");
                goto fail;
            }

            switch (parser->data[parser->pos])
            {
            case '"':
                g_string_append_c(out, '"');
                parser->pos++;
                break;
            case '\\':
                g_string_append_c(out, '\\');
                parser->pos++;
                break;
            case '/':
                g_string_append_c(out, '/');
                parser->pos++;
                break;
            case 'b':
                g_string_append_c(out, '\b');
                parser->pos++;
                break;
            case 'f':
                g_string_append_c(out, '\f');
                parser->pos++;
                break;
            case 'n':
                g_string_append_c(out, '\n');
                parser->pos++;
                break;
            case 'r':
                g_string_append_c(out, '\r');
                parser->pos++;
                break;
            case 't':
                g_string_append_c(out, '\t');
                parser->pos++;
                break;
            case 'u':
            {
                guint32 codepoint = 0;

                parser->pos++;

                if (!parse_hex4(parser, &codepoint))
                    goto fail;

                /* 处理 UTF-16 代理对 */
                if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
                    parser->pos + 1 < parser->length &&
                    parser->data[parser->pos] == '\\' &&
                    parser->data[parser->pos + 1] == 'u')
                {
                    const gsize saved = parser->pos;
                    guint32 low = 0;

                    parser->pos += 2;

                    if (parse_hex4(parser, &low) &&
                        low >= 0xdc00 && low <= 0xdfff)
                        codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                    else
                        parser->pos = saved;
                }

                append_codepoint(out, codepoint);
                break;
            }
            default:
                parse_fail(parser, "未知转义序列 '\\%c'", parser->data[parser->pos]);
                goto fail;
            }

            continue;
        }

        g_string_append_c(out, c);
        parser->pos++;
    }

    parse_fail(parser, "字符串未闭合");

fail:
    g_string_free(out, TRUE);
    return NULL;
}

static ZgJson *
parse_number(ZgJsonParser *parser)
{
    const gsize start = parser->pos;
    gsize end;
    char *text;
    char *endptr = NULL;
    ZgJson *node;

    while (parser->pos < parser->length)
    {
        const char c = parser->data[parser->pos];

        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E')
            parser->pos++;
        else
            break;
    }

    end = parser->pos;

    if (end == start)
    {
        parse_fail(parser, "期望数字");
        return NULL;
    }

    text = g_strndup(parser->data + start, end - start);
    node = zg_json_new(ZG_JSON_TYPE_NUMBER);
    node->u.number = g_ascii_strtod(text, &endptr);

    if (endptr == text)
    {
        parse_fail(parser, "非法数字 '%s'", text);
        g_free(text);
        zg_json_free(node);
        return NULL;
    }

    g_free(text);

    return node;
}

static ZgJson *
parse_array(ZgJsonParser *parser)
{
    ZgJson *node;

    if (!expect_char(parser, '['))
        return NULL;

    node = zg_json_new(ZG_JSON_TYPE_ARRAY);
    node->u.array = g_ptr_array_new();

    skip_whitespace(parser);

    if (parser->pos < parser->length && parser->data[parser->pos] == ']')
    {
        parser->pos++;
        return node;
    }

    for (;;)
    {
        ZgJson *element = parse_value(parser);

        if (element == NULL)
        {
            zg_json_free(node);
            return NULL;
        }

        g_ptr_array_add(node->u.array, element);

        skip_whitespace(parser);

        if (parser->pos >= parser->length)
        {
            parse_fail(parser, "数组未闭合");
            zg_json_free(node);
            return NULL;
        }

        if (parser->data[parser->pos] == ',')
        {
            parser->pos++;
            continue;
        }

        if (parser->data[parser->pos] == ']')
        {
            parser->pos++;
            return node;
        }

        parse_fail(parser, "数组中期望 ',' 或 ']'");
        zg_json_free(node);
        return NULL;
    }
}

static ZgJson *
parse_object(ZgJsonParser *parser)
{
    ZgJson *node;

    if (!expect_char(parser, '{'))
        return NULL;

    node = zg_json_new(ZG_JSON_TYPE_OBJECT);
    node->u.object = g_ptr_array_new();

    skip_whitespace(parser);

    if (parser->pos < parser->length && parser->data[parser->pos] == '}')
    {
        parser->pos++;
        return node;
    }

    for (;;)
    {
        char *key;
        ZgJson *value;
        ZgJsonMember *member;

        key = parse_string_literal(parser);

        if (key == NULL)
        {
            zg_json_free(node);
            return NULL;
        }

        if (!expect_char(parser, ':'))
        {
            g_free(key);
            zg_json_free(node);
            return NULL;
        }

        value = parse_value(parser);

        if (value == NULL)
        {
            g_free(key);
            zg_json_free(node);
            return NULL;
        }

        member = g_new0(ZgJsonMember, 1);
        member->key = key;
        member->value = value;
        g_ptr_array_add(node->u.object, member);

        skip_whitespace(parser);

        if (parser->pos >= parser->length)
        {
            parse_fail(parser, "对象未闭合");
            zg_json_free(node);
            return NULL;
        }

        if (parser->data[parser->pos] == ',')
        {
            parser->pos++;
            continue;
        }

        if (parser->data[parser->pos] == '}')
        {
            parser->pos++;
            return node;
        }

        parse_fail(parser, "对象中期望 ',' 或 '}'");
        zg_json_free(node);
        return NULL;
    }
}

static ZgJson *
parse_value(ZgJsonParser *parser)
{
    skip_whitespace(parser);

    if (parser->pos >= parser->length)
    {
        parse_fail(parser, "意外的输入结束");
        return NULL;
    }

    if (++parser->depth > ZG_JSON_MAX_DEPTH)
    {
        parse_fail(parser, "嵌套层级超过 %d", ZG_JSON_MAX_DEPTH);
        return NULL;
    }

    {
        ZgJson *node = NULL;

        switch (parser->data[parser->pos])
        {
        case '{':
            node = parse_object(parser);
            break;
        case '[':
            node = parse_array(parser);
            break;
        case '"':
        {
            char *string = parse_string_literal(parser);

            if (string != NULL)
            {
                node = zg_json_new(ZG_JSON_TYPE_STRING);
                node->u.string = string;
            }
            break;
        }
        case 't':
            if (g_str_has_prefix(parser->data + parser->pos, "true"))
            {
                parser->pos += 4;
                node = zg_json_new(ZG_JSON_TYPE_BOOL);
                node->u.boolean = TRUE;
            }
            else
            {
                parse_fail(parser, "非法字面量");
            }
            break;
        case 'f':
            if (g_str_has_prefix(parser->data + parser->pos, "false"))
            {
                parser->pos += 5;
                node = zg_json_new(ZG_JSON_TYPE_BOOL);
                node->u.boolean = FALSE;
            }
            else
            {
                parse_fail(parser, "非法字面量");
            }
            break;
        case 'n':
            if (g_str_has_prefix(parser->data + parser->pos, "null"))
            {
                parser->pos += 4;
                node = zg_json_new(ZG_JSON_TYPE_NULL);
            }
            else
            {
                parse_fail(parser, "非法字面量");
            }
            break;
        default:
            node = parse_number(parser);
            break;
        }

        parser->depth--;

        return node;
    }
}

ZgJson *
zg_json_parse(const char *text,
              gssize len,
              GError **error)
{
    ZgJsonParser parser;
    ZgJson *node;

    g_return_val_if_fail(text != NULL, NULL);

    parser.data = text;
    parser.length = (len < 0) ? strlen(text) : (gsize)len;
    parser.pos = 0;
    parser.depth = 0;
    parser.error = error;
    parser.failed = FALSE;

    node = parse_value(&parser);

    if (node == NULL)
        return NULL;

    skip_whitespace(&parser);

    if (parser.pos != parser.length)
    {
        parse_fail(&parser, "JSON 之后存在多余内容");
        zg_json_free(node);
        return NULL;
    }

    return node;
}

/* ------------------------------------------------------------------------- */
/* DOM 访问器                                                                 */
/* ------------------------------------------------------------------------- */

ZgJsonType
zg_json_get_type(const ZgJson *node)
{
    return (node != NULL) ? node->type : ZG_JSON_TYPE_NULL;
}

gboolean
zg_json_is_null(const ZgJson *node)
{
    return node == NULL || node->type == ZG_JSON_TYPE_NULL;
}

const char *
zg_json_get_string(const ZgJson *node)
{
    if (node == NULL || node->type != ZG_JSON_TYPE_STRING)
        return NULL;

    return node->u.string;
}

gboolean
zg_json_get_bool(const ZgJson *node,
                 gboolean fallback)
{
    if (node == NULL || node->type != ZG_JSON_TYPE_BOOL)
        return fallback;

    return node->u.boolean;
}

gint64
zg_json_get_int(const ZgJson *node,
                gint64 fallback)
{
    if (node == NULL)
        return fallback;

    if (node->type == ZG_JSON_TYPE_NUMBER)
        return (gint64)node->u.number;

    if (node->type == ZG_JSON_TYPE_STRING)
    {
        gint64 value = 0;

        if (g_ascii_string_to_signed(node->u.string, 10, G_MININT64, G_MAXINT64,
                                     &value, NULL))
            return value;
    }

    return fallback;
}

guint zg_json_array_size(const ZgJson *node)
{
    if (node == NULL || node->type != ZG_JSON_TYPE_ARRAY || node->u.array == NULL)
        return 0;

    return node->u.array->len;
}

const ZgJson *
zg_json_array_at(const ZgJson *node,
                 guint index)
{
    if (node == NULL || node->type != ZG_JSON_TYPE_ARRAY || node->u.array == NULL)
        return NULL;

    if (index >= node->u.array->len)
        return NULL;

    return g_ptr_array_index(node->u.array, index);
}

const ZgJson *
zg_json_object_get(const ZgJson *node,
                   const char *key)
{
    guint i;

    if (node == NULL || node->type != ZG_JSON_TYPE_OBJECT ||
        node->u.object == NULL || key == NULL)
        return NULL;

    for (i = 0; i < node->u.object->len; i++)
    {
        const ZgJsonMember *member = g_ptr_array_index(node->u.object, i);

        if (g_strcmp0(member->key, key) == 0)
            return member->value;
    }

    return NULL;
}

guint zg_json_object_size(const ZgJson *node)
{
    if (node == NULL || node->type != ZG_JSON_TYPE_OBJECT || node->u.object == NULL)
        return 0;

    return node->u.object->len;
}

const char *
zg_json_object_key_at(const ZgJson *node,
                      guint index)
{
    const ZgJsonMember *member;

    if (node == NULL || node->type != ZG_JSON_TYPE_OBJECT || node->u.object == NULL)
        return NULL;

    if (index >= node->u.object->len)
        return NULL;

    member = g_ptr_array_index(node->u.object, index);

    return member->key;
}

const ZgJson *
zg_json_object_value_at(const ZgJson *node,
                        guint index)
{
    const ZgJsonMember *member;

    if (node == NULL || node->type != ZG_JSON_TYPE_OBJECT || node->u.object == NULL)
        return NULL;

    if (index >= node->u.object->len)
        return NULL;

    member = g_ptr_array_index(node->u.object, index);

    return member->value;
}

const char *
zg_json_object_get_string(const ZgJson *node,
                          const char *key)
{
    return zg_json_get_string(zg_json_object_get(node, key));
}

const char *
zg_json_object_get_string_or(const ZgJson *node,
                             const char *key,
                             const char *fallback)
{
    const char *value = zg_json_object_get_string(node, key);

    return (value != NULL) ? value : fallback;
}

gboolean
zg_json_object_get_bool(const ZgJson *node,
                        const char *key,
                        gboolean fallback)
{
    return zg_json_get_bool(zg_json_object_get(node, key), fallback);
}

gint64
zg_json_object_get_int(const ZgJson *node,
                       const char *key,
                       gint64 fallback)
{
    return zg_json_get_int(zg_json_object_get(node, key), fallback);
}

/* ------------------------------------------------------------------------- */
/* 写入器                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct
{
    gboolean is_object;
    gboolean first;
} ZgJsonLevel;

struct _ZgJsonWriter
{
    GString *buffer;
    GArray *stack; /* ZgJsonLevel */
};

ZgJsonWriter *
zg_json_writer_new(void)
{
    ZgJsonWriter *writer = g_new0(ZgJsonWriter, 1);

    writer->buffer = g_string_new(NULL);
    writer->stack = g_array_new(FALSE, FALSE, sizeof(ZgJsonLevel));

    return writer;
}

void zg_json_writer_free(ZgJsonWriter *writer)
{
    if (writer == NULL)
        return;

    if (writer->buffer != NULL)
        g_string_free(writer->buffer, TRUE);

    if (writer->stack != NULL)
        g_array_free(writer->stack, TRUE);

    g_free(writer);
}

void zg_json_escape(GString *out,
                    const char *str)
{
    const char *p;

    g_string_append_c(out, '"');

    if (str != NULL)
    {
        for (p = str; *p != '\0'; p++)
        {
            const guchar c = (guchar)*p;

            switch (c)
            {
            case '"':
                g_string_append(out, "\\\"");
                break;
            case '\\':
                g_string_append(out, "\\\\");
                break;
            case '\b':
                g_string_append(out, "\\b");
                break;
            case '\f':
                g_string_append(out, "\\f");
                break;
            case '\n':
                g_string_append(out, "\\n");
                break;
            case '\r':
                g_string_append(out, "\\r");
                break;
            case '\t':
                g_string_append(out, "\\t");
                break;
            default:
                if (c < 0x20)
                    g_string_append_printf(out, "\\u%04x", (guint)c);
                else
                    g_string_append_c(out, (char)c);
                break;
            }
        }
    }

    g_string_append_c(out, '"');
}

static ZgJsonLevel *
writer_top(ZgJsonWriter *writer)
{
    if (writer->stack->len == 0)
        return NULL;

    return &g_array_index(writer->stack, ZgJsonLevel, writer->stack->len - 1);
}

static void
writer_prepare(ZgJsonWriter *writer,
               const char *key)
{
    ZgJsonLevel *level = writer_top(writer);

    if (level == NULL)
        return; /* 顶层值 */

    if (level->is_object)
    {
        if (key == NULL)
            return; /* 调用方错误，忽略该值以免生成非法 JSON */

        if (!level->first)
            g_string_append_c(writer->buffer, ',');

        zg_json_escape(writer->buffer, key);
        g_string_append_c(writer->buffer, ':');
    }
    else
    {
        if (!level->first)
            g_string_append_c(writer->buffer, ',');
    }

    level->first = FALSE;
}

static void
writer_push(ZgJsonWriter *writer,
            gboolean is_object)
{
    ZgJsonLevel level;

    level.is_object = is_object;
    level.first = TRUE;
    g_array_append_val(writer->stack, level);
}

static void
writer_pop(ZgJsonWriter *writer)
{
    if (writer->stack->len > 0)
        g_array_set_size(writer->stack, writer->stack->len - 1);
}

void zg_json_writer_begin_object(ZgJsonWriter *writer,
                                 const char *key)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    g_string_append_c(writer->buffer, '{');
    writer_push(writer, TRUE);
}

void zg_json_writer_end_object(ZgJsonWriter *writer)
{
    g_return_if_fail(writer != NULL);

    writer_pop(writer);
    g_string_append_c(writer->buffer, '}');
}

void zg_json_writer_begin_array(ZgJsonWriter *writer,
                                const char *key)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    g_string_append_c(writer->buffer, '[');
    writer_push(writer, FALSE);
}

void zg_json_writer_end_array(ZgJsonWriter *writer)
{
    g_return_if_fail(writer != NULL);

    writer_pop(writer);
    g_string_append_c(writer->buffer, ']');
}

void zg_json_writer_set_string(ZgJsonWriter *writer,
                               const char *key,
                               const char *value)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    zg_json_escape(writer->buffer, value);
}

void zg_json_writer_set_int(ZgJsonWriter *writer,
                            const char *key,
                            gint64 value)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    g_string_append_printf(writer->buffer, "%" G_GINT64_FORMAT, value);
}

void zg_json_writer_set_bool(ZgJsonWriter *writer,
                             const char *key,
                             gboolean value)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    g_string_append(writer->buffer, value ? "true" : "false");
}

void zg_json_writer_set_null(ZgJsonWriter *writer,
                             const char *key)
{
    g_return_if_fail(writer != NULL);

    writer_prepare(writer, key);
    g_string_append(writer->buffer, "null");
}

void zg_json_writer_set_raw(ZgJsonWriter *writer,
                            const char *key,
                            const char *raw_json)
{
    g_return_if_fail(writer != NULL);
    g_return_if_fail(raw_json != NULL);

    writer_prepare(writer, key);
    g_string_append(writer->buffer, raw_json);
}

void zg_json_writer_add_string(ZgJsonWriter *writer,
                               const char *value)
{
    /* 数组元素没有 key，直接复用 set_string 的 NULL 语义 */
    zg_json_writer_set_string(writer, NULL, value);
}

void zg_json_writer_add_int(ZgJsonWriter *writer,
                            gint64 value)
{
    zg_json_writer_set_int(writer, NULL, value);
}

void zg_json_writer_add_raw(ZgJsonWriter *writer,
                            const char *raw_json)
{
    zg_json_writer_set_raw(writer, NULL, raw_json);
}

char *
zg_json_writer_to_string(ZgJsonWriter *writer,
                         gsize *length)
{
    char *result;

    g_return_val_if_fail(writer != NULL, NULL);
    g_return_val_if_fail(writer->buffer != NULL, NULL);

    result = g_string_free(writer->buffer, FALSE);
    writer->buffer = NULL;

    if (length != NULL)
        *length = (result != NULL) ? strlen(result) : 0;

    zg_json_writer_free(writer);

    return result;
}

/* ------------------------------------------------------------------------- */
/* 重新序列化                                                                 */
/* ------------------------------------------------------------------------- */

static void
serialize_node(ZgJsonWriter *writer,
               const char *key,
               const ZgJson *node)
{
    guint i;

    switch (zg_json_get_type(node))
    {
    case ZG_JSON_TYPE_NULL:
        zg_json_writer_set_null(writer, key);
        break;
    case ZG_JSON_TYPE_BOOL:
        zg_json_writer_set_bool(writer, key, node->u.boolean);
        break;
    case ZG_JSON_TYPE_NUMBER:
        zg_json_writer_set_int(writer, key, (gint64)node->u.number);
        break;
    case ZG_JSON_TYPE_STRING:
        zg_json_writer_set_string(writer, key, node->u.string);
        break;
    case ZG_JSON_TYPE_ARRAY:
        zg_json_writer_begin_array(writer, key);

        for (i = 0; i < zg_json_array_size(node); i++)
            serialize_node(writer, NULL, zg_json_array_at(node, i));

        zg_json_writer_end_array(writer);
        break;
    case ZG_JSON_TYPE_OBJECT:
        zg_json_writer_begin_object(writer, key);

        for (i = 0; i < zg_json_object_size(node); i++)
            serialize_node(writer,
                           zg_json_object_key_at(node, i),
                           zg_json_object_value_at(node, i));

        zg_json_writer_end_object(writer);
        break;
    }
}

char *
zg_json_to_text(const ZgJson *node)
{
    ZgJsonWriter *writer;
    char *text;

    writer = zg_json_writer_new();
    serialize_node(writer, NULL, node);
    text = zg_json_writer_to_string(writer, NULL);

    return text;
}
