#ifndef SPISOR_TEST_JSON_OF_H
#define SPISOR_TEST_JSON_OF_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "naula/json.h"

/** The tree of `length` bytes as naula reads JSON, NULL for bytes that are not JSON or when memory runs out. */
static inline cJSON *json_of(const void *bytes, size_t length) {
    cJSON *tree = NULL;
    const char *reason = NULL;
    naula_json_parse_keeping_last_value((const uint8_t *)bytes, length, &tree, &reason);
    return tree;
}

#endif
