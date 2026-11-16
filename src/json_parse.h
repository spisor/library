#ifndef SPISOR_JSON_PARSE_H
#define SPISOR_JSON_PARSE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** spisor_parse_json, setting *out_of_memory when it answered NULL because memory ran out rather than for the text. */
cJSON *spisor_parse_json_reporting(const uint8_t *bytes, size_t length, int *out_of_memory);

#endif
