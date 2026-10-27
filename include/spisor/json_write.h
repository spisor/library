#ifndef SPISOR_JSON_WRITE_H
#define SPISOR_JSON_WRITE_H

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

#ifdef __cplusplus
}
#endif

#endif
