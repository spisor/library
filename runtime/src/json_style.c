#include "json_style.h"

#include <stdlib.h>
#include <string.h>

#include "spisor/json_write.h"

typedef struct growing_text {
    char *bytes;
    size_t length;
    size_t capacity;
    int failed;
} growing_text;

static const json_text two_spaces = {"  ", 2};
static const json_text nothing = {"", 0};

static void append(growing_text *out, const char *bytes, size_t count) {
    if (out->failed) {
        return;
    }
    if (out->length + count + 1 > out->capacity) {
        size_t capacity = (out->length + count + 1) * 2;
        char *grown = realloc(out->bytes, capacity);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->bytes = grown;
        out->capacity = capacity;
    }
    memcpy(out->bytes + out->length, bytes, count);
    out->length += count;
    out->bytes[out->length] = '\0';
}

static void append_text(growing_text *out, json_text text) {
    append(out, text.bytes, text.length);
}

static char *finished(growing_text *out, size_t *length) {
    if (out->failed || out->bytes == NULL) {
        free(out->bytes);
        return NULL;
    }
    *length = out->length;
    return out->bytes;
}

static int is_indent_byte(char character) {
    return character == ' ' || character == '\t';
}

static json_text text_between(const char *text, size_t start, size_t end) {
    json_text between = {text + start, end - start};
    return between;
}

/** The spaces and tabs that start the line holding at. */
static json_text line_indent(const char *text, size_t at) {
    size_t start = at;
    while (start > 0 && text[start - 1] != '\n') {
        start--;
    }
    size_t end = start;
    while (end < at && is_indent_byte(text[end])) {
        end++;
    }
    return text_between(text, start, end);
}

static int first_member_of(const char *text, json_span object, json_member *member) {
    json_members walk;
    json_members_start(&walk, text, object.end, object);
    return json_members_next(&walk, member) == 1;
}

/** Whether the member's key starts its own line after the brace, with that line's indent and ending. */
static int starts_own_line(const char *text, json_span object, json_span key, json_style *style) {
    size_t start = key.start;
    while (start > object.start + 1 && is_indent_byte(text[start - 1])) {
        start--;
    }
    if (start <= object.start + 1 || text[start - 1] != '\n') {
        return 0;
    }
    style->indent_prefix = text_between(text, start, key.start);
    style->newline = start >= 2 && text[start - 2] == '\r' ? "\r\n" : "\n";
    return 1;
}

static json_text member_separator(const char *text, const json_member *member) {
    json_text between = text_between(text, member->key.end, member->value.start);
    int plain = memchr(between.bytes, '\n', between.length) == NULL && memchr(between.bytes, '/', between.length) == NULL;
    json_text spaced = {": ", 2};
    return plain ? between : spaced;
}

static json_text single_line_gap(const char *text, json_span object, json_span key) {
    for (size_t index = object.start + 1; index < key.start; index++) {
        if (!is_indent_byte(text[index])) {
            return nothing;
        }
    }
    return text_between(text, object.start + 1, key.start);
}

/** indent less the leading line indent, when indent extends it; empty otherwise. */
static json_text unit_beyond(json_text indent, json_text line) {
    if (indent.length <= line.length || memcmp(indent.bytes, line.bytes, line.length) != 0) {
        return nothing;
    }
    json_text unit = {indent.bytes + line.length, indent.length - line.length};
    return unit;
}

static json_style compact_style(void) {
    json_style style = {0, "\n", {"", 0}, {"", 0}, {"", 0}, {"", 0}, {":", 1}, {"", 0}};
    return style;
}

/** The style of an object with members, its unit falling back to fallback_unit when it cannot be read. */
static json_style filled_style(const char *text, json_span object, const json_member *first, json_text fallback_unit) {
    json_style style = compact_style();
    style.separator = member_separator(text, first);
    if (!starts_own_line(text, object, first->key, &style)) {
        style.gap = single_line_gap(text, object, first->key);
        return style;
    }
    style.multiline = 1;
    style.unit = unit_beyond(style.indent_prefix, line_indent(text, object.start));
    if (style.unit.length == 0) {
        style.unit = fallback_unit;
    }
    return style;
}

static json_style empty_style(const char *text, json_span root, json_span object) {
    json_member first;
    if (!first_member_of(text, root, &first)) {
        return compact_style();
    }
    json_style root_style = filled_style(text, root, &first, two_spaces);
    if (!root_style.multiline) {
        json_style style = compact_style();
        style.separator = root_style.separator;
        return style;
    }
    json_style style = root_style;
    style.indent_prefix = line_indent(text, object.start);
    style.indent_suffix = root_style.unit;
    style.closing = style.indent_prefix;
    return style;
}

json_style json_style_of(const char *text, json_span root, json_span object) {
    json_member first;
    if (!first_member_of(text, object, &first)) {
        return empty_style(text, root, object);
    }
    json_member root_first;
    json_text fallback = two_spaces;
    if (object.start != root.start && first_member_of(text, root, &root_first)) {
        json_style root_style = filled_style(text, root, &root_first, two_spaces);
        fallback = root_style.multiline ? root_style.unit : two_spaces;
    }
    return filled_style(text, object, &first, fallback);
}

static void append_line_break(growing_text *out, const json_style *style) {
    append(out, style->newline, strlen(style->newline));
    append_text(out, style->indent_prefix);
    append_text(out, style->indent_suffix);
}

/** JSON.stringify's two-space form with each line after the first indented by the style instead. */
static void append_reindented(growing_text *out, const char *pretty, const json_style *style) {
    const char *line = pretty;
    for (int first = 1;; first = 0) {
        const char *line_feed = strchr(line, '\n');
        size_t line_length = line_feed == NULL ? strlen(line) : (size_t)(line_feed - line);
        if (!first) {
            size_t spaces = 0;
            while (spaces < line_length && line[spaces] == ' ') {
                spaces++;
            }
            append_line_break(out, style);
            for (size_t level = 0; level < spaces / 2; level++) {
                append_text(out, style->unit);
            }
            line += spaces;
            line_length -= spaces;
        }
        append(out, line, line_length);
        if (line_feed == NULL) {
            return;
        }
        line = line_feed + 1;
    }
}

/** JSON.stringify(value) from its two-space form: every space and line feed outside a string dropped. */
static void append_compact(growing_text *out, const char *pretty) {
    int in_string = 0;
    for (const char *cursor = pretty; *cursor != '\0'; cursor++) {
        if (in_string && *cursor == '\\') {
            append(out, cursor, 2);
            cursor++;
            continue;
        }
        if (*cursor == '"') {
            in_string = !in_string;
        }
        if (in_string || (*cursor != ' ' && *cursor != '\n')) {
            append(out, cursor, 1);
        }
    }
}

static void append_value(growing_text *out, const json_style *style, const cJSON *value) {
    char *pretty = spisor_json_stringify_pretty(value);
    if (pretty == NULL) {
        out->failed = 1;
        return;
    }
    if (style->multiline) {
        append_reindented(out, pretty, style);
    } else {
        append_compact(out, pretty);
    }
    free(pretty);
}

static void append_key(growing_text *out, const char *name) {
    cJSON *key = cJSON_CreateString(name);
    char *quoted = key == NULL ? NULL : spisor_json_stringify_pretty(key);
    cJSON_Delete(key);
    if (quoted == NULL) {
        out->failed = 1;
        return;
    }
    append(out, quoted, strlen(quoted));
    free(quoted);
}

char *json_style_member_text(const json_style *style, json_placement placement, const char *name,
                             const cJSON *value, size_t *length) {
    growing_text out = {NULL, 0, 0, 0};
    append(&out, "", 0);
    if (placement == JSON_PLACED_AFTER_MEMBER) {
        append(&out, ",", 1);
    }
    if (style->multiline) {
        append_line_break(&out, style);
    } else if (placement == JSON_PLACED_AFTER_MEMBER) {
        append_text(&out, style->gap);
    }
    append_key(&out, name);
    append_text(&out, style->separator);
    append_value(&out, style, value);
    if (style->multiline && placement == JSON_PLACED_ALONE) {
        append(&out, style->newline, strlen(style->newline));
        append_text(&out, style->closing);
    }
    return finished(&out, length);
}

char *json_style_value_text(const json_style *style, const cJSON *value, size_t *length) {
    growing_text out = {NULL, 0, 0, 0};
    append(&out, "", 0);
    append_value(&out, style, value);
    return finished(&out, length);
}
