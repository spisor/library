#ifndef SPISOR_JSON_WRITE_H
#define SPISOR_JSON_WRITE_H

#include <stddef.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Exactly what JSON.stringify(value, null, 2) returns for the same parsed value, without a
 * trailing newline. Freed with free; NULL only when memory runs out.
 *
 * @implNote Object keys come out in JavaScript's own order: array-index keys (0 to 4294967294, no
 * leading zero) ascending, then the rest in tree order. A number is written by
 * spisor_javascript_string, so -0 is 0, and one that is not finite is null.
 */
char *spisor_json_stringify_pretty(const cJSON *value);

/**
 * The members of object in the order JavaScript enumerates them, the order spisor_json_stringify_pretty
 * writes. Freed with free; NULL with *count 0 for an empty object or for NULL, and NULL
 * with *count nonzero when memory runs out.
 */
const cJSON **spisor_json_members_in_javascript_order(const cJSON *object, size_t *count);

#ifdef __cplusplus
}
#endif

#endif
