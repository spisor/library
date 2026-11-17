#ifndef RUNTIME_NODE_JSON_FILE_H
#define RUNTIME_NODE_JSON_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * JSON.parse(readFileSync(path, "utf8")): the file decoded as Node decodes UTF-8, then parsed with no comment
 * stripping. NULL when it cannot be read or is not JSON, *out_of_memory set to 1 when memory ran out reading it and to
 * 0 otherwise.
 */
cJSON *runtime_read_node_json(const char *path, int *out_of_memory);

/** runtime_read_node_json over bytes already read: NULL when they are not JSON, *out_of_memory as it sets it. */
cJSON *runtime_parse_node_json(const uint8_t *bytes, size_t length, int *out_of_memory);

/** Whether every member of object is a string, the shape of an id-to-version map. */
int runtime_is_version_map(const cJSON *value);

#endif
