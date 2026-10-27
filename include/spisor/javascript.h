#ifndef SPISOR_JAVASCRIPT_H
#define SPISOR_JAVASCRIPT_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Node's Buffer.toString("utf8") (and readFileSync(file, "utf8")): the bytes decoded with each
 * maximal invalid subpart replaced by U+FFFD, re-encoded as well-formed UTF-8. Freed with free,
 * NULL only when memory runs out; the buffer carries one NUL past out_length.
 *
 * @implNote The WHATWG decoder's table: the byte that ends an invalid subpart is read again as the
 * start of what follows, which is what Node does since it moved to simdutf.
 */
uint8_t *spisor_decode_utf8_lossy(const uint8_t *bytes, size_t length, size_t *out_length);

/**
 * The code point starting at text[*index] of well-formed UTF-8, advancing *index past it. Only for
 * text already validated or produced by spisor_decode_utf8_lossy.
 */
uint32_t spisor_utf8_next(const char *text, size_t *index);

/** JavaScript's WhiteSpace and LineTerminator: what a regex \s and String.prototype.trim match. */
int spisor_is_javascript_space(uint32_t code_point);

/** String.prototype.trim on well-formed UTF-8, freed with free; NULL only when memory runs out. */
char *spisor_javascript_trim(const char *text);

/**
 * JavaScript's String(value) for a parsed JSON value, which is also what the value becomes when
 * used as a property key: a number in Number.prototype.toString's shortest form, an array by
 * Array.prototype.join (null elements empty), an object as "[object Object]". Freed with free.
 */
char *spisor_javascript_string(const cJSON *value);

#ifdef __cplusplus
}
#endif

#endif
