#ifndef RUNTIME_JSON_STYLE_H
#define RUNTIME_JSON_STYLE_H

#include <stddef.h>

#include "cJSON.h"
#include "json_span.h"

typedef struct json_text {
    const char *bytes;
    size_t length;
} json_text;

/**
 * How a member added to one object is written so it reads like the members already there.
 *
 * An object whose first member starts on a line of its own is multiline: a member is written on a
 * new line with the first member's indentation and line ending, and its value as JSON.stringify's
 * two-space form re-indented by unit, the first member's indentation less the indentation of the
 * line holding the object's brace. Any other object with members is single line: ", " or "," as
 * its first member follows its brace, and the value compact. Every member is joined to its value
 * as the object's first member is. An empty object takes the root object's style one level deeper
 * than the line holding its brace, or is compact when the root has no multiline member to read.
 */
typedef struct json_style {
    int multiline;
    const char *newline;
    json_text indent_prefix;
    json_text indent_suffix;
    json_text closing;
    json_text unit;
    json_text separator;
    json_text gap;
} json_style;

/** The style for a member added to object, a span inside root, the document's root object. */
json_style json_style_of(const char *text, json_span root, json_span object);

/** Where an added member's bytes go, which decides what they carry besides the member itself. */
typedef enum json_placement {
    JSON_PLACED_AFTER_MEMBER,
    JSON_PLACED_ALONE,
    JSON_PLACED_ABOVE_CLOSING_LINE
} json_placement;

/**
 * The bytes that add a member name holding value, freed with free; NULL when memory runs out. AFTER_MEMBER
 * bytes go after the last member's value, comma first. In an object with no member, ALONE bytes stand
 * between its braces and, when multiline, bring the closing brace's line; ABOVE_CLOSING_LINE bytes go
 * before the line break already ending what is inside, so they bring only the member's own line.
 */
char *json_style_member_text(const json_style *style, json_placement placement, const char *name,
                             const cJSON *value, size_t *length);

/** value written as a member's value in style, for replacing the bytes of one; NULL when memory runs out. */
char *json_style_value_text(const json_style *style, const cJSON *value, size_t *length);

#endif
