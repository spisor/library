#include "json_span.h"

#include <stdlib.h>
#include <string.h>

#include "spisor/json_kind.h"

/** Deeper than cJSON's own nesting limit, so a text spisor_read_json_file parsed is never refused here. */
#define DEEPEST_NESTING 1100

static int is_space(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static int is_comment_start(const char *text, size_t length, size_t at) {
    return at + 1 < length && text[at] == '/' && text[at + 1] == '/';
}

static int is_bom(const char *text, size_t length, size_t at) {
    return at == 0 && length >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0;
}

size_t json_span_skip_space(const char *text, size_t length, size_t at) {
    if (is_bom(text, length, at)) {
        at = 3;
    }
    while (at < length) {
        if (is_comment_start(text, length, at)) {
            const char *line_feed = memchr(text + at, '\n', length - at);
            at = line_feed == NULL ? length : (size_t)(line_feed - text);
        } else if (is_space(text[at])) {
            at++;
        } else {
            break;
        }
    }
    return at;
}

/** The index just past the string whose opening quote is at, or 0 when it never closes. */
static size_t string_end(const char *text, size_t length, size_t at) {
    for (size_t index = at + 1; index < length; index++) {
        if (text[index] == '\\') {
            index++;
        } else if (text[index] == '"') {
            return index + 1;
        }
    }
    return 0;
}

static int is_scalar_byte(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') || character == '-' ||
           character == '+' || character == '.' || character == 'E';
}

static size_t scalar_end(const char *text, size_t length, size_t at) {
    size_t index = at;
    while (index < length && is_scalar_byte(text[index])) {
        index++;
    }
    return index > at ? index : 0;
}

static size_t value_end(const char *text, size_t length, size_t at, int depth);

/** The index just past the array or object opening at, or 0 when it is malformed. */
static size_t container_end(const char *text, size_t length, size_t at, int depth) {
    char closing = text[at] == '{' ? '}' : ']';
    size_t cursor = json_span_skip_space(text, length, at + 1);
    if (cursor < length && text[cursor] == closing) {
        return cursor + 1;
    }
    while (cursor < length && depth < DEEPEST_NESTING) {
        if (closing == '}') {
            size_t key_end = text[cursor] == '"' ? string_end(text, length, cursor) : 0;
            cursor = key_end == 0 ? length : json_span_skip_space(text, length, key_end);
            if (cursor >= length || text[cursor] != ':') {
                return 0;
            }
            cursor = json_span_skip_space(text, length, cursor + 1);
        }
        size_t end = cursor < length ? value_end(text, length, cursor, depth + 1) : 0;
        if (end == 0) {
            return 0;
        }
        cursor = json_span_skip_space(text, length, end);
        if (cursor < length && text[cursor] == closing) {
            return cursor + 1;
        }
        if (cursor >= length || text[cursor] != ',') {
            return 0;
        }
        cursor = json_span_skip_space(text, length, cursor + 1);
    }
    return 0;
}

static size_t value_end(const char *text, size_t length, size_t at, int depth) {
    if (text[at] == '"') {
        return string_end(text, length, at);
    }
    if (text[at] == '{' || text[at] == '[') {
        return container_end(text, length, at, depth);
    }
    return scalar_end(text, length, at);
}

int json_span_value(const char *text, size_t length, size_t at, json_span *value) {
    size_t start = json_span_skip_space(text, length, at);
    size_t end = start < length ? value_end(text, length, start, 0) : 0;
    if (end == 0) {
        return 0;
    }
    value->start = start;
    value->end = end;
    return 1;
}

void json_members_start(json_members *walk, const char *text, size_t length, json_span object) {
    walk->text = text;
    walk->length = length;
    walk->cursor = object.start + 1;
    walk->object_end = object.end - 1;
    walk->count = 0;
}

/** Moves past the comma before the next member; 0 at the object's end, -1 when neither follows. */
static int pass_separator(json_members *walk) {
    size_t cursor = json_span_skip_space(walk->text, walk->object_end, walk->cursor);
    if (cursor >= walk->object_end) {
        return 0;
    }
    if (walk->count > 0) {
        if (walk->text[cursor] != ',') {
            return -1;
        }
        cursor = json_span_skip_space(walk->text, walk->object_end, cursor + 1);
    }
    walk->cursor = cursor;
    return cursor < walk->object_end ? 1 : -1;
}

int json_members_next(json_members *walk, json_member *member) {
    int found = pass_separator(walk);
    if (found <= 0) {
        return found;
    }
    const char *text = walk->text;
    size_t key_end = text[walk->cursor] == '"' ? string_end(text, walk->object_end, walk->cursor) : 0;
    size_t colon = key_end == 0 ? walk->object_end : json_span_skip_space(text, walk->object_end, key_end);
    if (colon >= walk->object_end || text[colon] != ':' ||
        !json_span_value(text, walk->object_end, colon + 1, &member->value)) {
        return -1;
    }
    member->key.start = walk->cursor;
    member->key.end = key_end;
    walk->cursor = member->value.end;
    walk->count++;
    return 1;
}

static int has_escape(const char *text, json_span key) {
    return memchr(text + key.start, '\\', key.end - key.start) != NULL;
}

int json_span_key_is(const char *text, json_span key, const char *name) {
    size_t inner = key.end - key.start - 2;
    if (!has_escape(text, key)) {
        return strlen(name) == inner && memcmp(text + key.start + 1, name, inner) == 0;
    }
    cJSON *decoded = spisor_parse_json((const uint8_t *)text + key.start, key.end - key.start);
    if (decoded == NULL) {
        return -1;
    }
    const char *decoded_name = cJSON_GetStringValue(decoded);
    int same = decoded_name != NULL && strcmp(decoded_name, name) == 0;
    cJSON_Delete(decoded);
    return same;
}
