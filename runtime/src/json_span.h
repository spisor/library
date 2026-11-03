#ifndef RUNTIME_JSON_SPAN_H
#define RUNTIME_JSON_SPAN_H

#include <stddef.h>

/** Bytes [start, end) of a JSON text. */
typedef struct json_span {
    size_t start;
    size_t end;
} json_span;

typedef struct json_member {
    json_span key;
    json_span value;
} json_member;

/** A walk over the members of one object, in the order the text holds them. */
typedef struct json_members {
    const char *text;
    size_t length;
    size_t cursor;
    size_t object_end;
    int count;
} json_members;

/**
 * Where the next token starts at or after at: past JSON whitespace, a leading UTF-8 BOM, and
 * whole-line // comments.
 *
 * @implNote The scanner only reads text spisor_read_json_file already parsed, which strips a // comment
 * only when it is alone on its line, so a // met where whitespace may stand always runs to its line end.
 */
size_t json_span_skip_space(const char *text, size_t length, size_t at);

/** The span of the value starting at or after at, past whitespace; 0 when the text there is malformed. */
int json_span_value(const char *text, size_t length, size_t at, json_span *value);

/** Starts a walk over object, a span json_span_value answered that starts with '{'. */
void json_members_start(json_members *walk, const char *text, size_t length, json_span object);

/** 1 with the next member, 0 past the last one, -1 when the text is malformed. */
int json_members_next(json_members *walk, json_member *member);

/** Whether the key token at key, quotes included, decodes to name; -1 when it cannot be decoded. */
int json_span_key_is(const char *text, json_span key, const char *name);

#endif
