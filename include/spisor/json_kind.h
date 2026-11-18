#ifndef SPISOR_JSON_KIND_H
#define SPISOR_JSON_KIND_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPISOR_STATUS_SUCCESS 0
#define SPISOR_STATUS_FAILURE 1
#define SPISOR_STATUS_OUT_OF_MEMORY 2

/**
 * Reads one whole JSON document exactly as JSON.parse would, or answers NULL. Refuses invalid
 * UTF-8, anything after the value, and a string (key or value, at any depth) holding a NUL.
 *
 * @implNote cJSON departs from JSON.parse in these ways, each closed here without touching the
 * vendored file: it accepts invalid UTF-8, so the bytes are validated first; its grammar is laxer
 * (a BOM, any byte up to 0x20 as whitespace, raw control characters in a string, numbers such as
 * 01 and 1.), so JSON.parse's grammar is checked before cJSON reads anything; it truncates a string
 * at an escaped NUL, so such a document is refused rather than read short; and it refuses an
 * unpaired surrogate escape, which JSON.stringify emits for an unpaired UTF-16 value, so each one
 * is rewritten to the escape of U+FFFD (what Node's UTF-8 encoding of that string holds) before
 * cJSON reads it.
 * A duplicated key keeps its FIRST position and its LAST value, as JSON.parse does, so every cJSON
 * lookup and iteration over the tree sees what JavaScript sees.
 */
cJSON *spisor_parse_json(const uint8_t *bytes, size_t length);

/** Frees a buffer this library wrote to out; a library's own release function calls it. */
void spisor_release(uint8_t *buffer);

#ifdef __cplusplus
}
#endif

#endif
