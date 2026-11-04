#ifndef RUNTIME_NODE_JSON_FILE_H
#define RUNTIME_NODE_JSON_FILE_H

#include "cJSON.h"

/**
 * JSON.parse(readFileSync(path, "utf8")): the file decoded as Node decodes UTF-8, then parsed with no
 * comment stripping. NULL when it cannot be read, is not JSON, or memory runs out.
 */
cJSON *runtime_read_node_json(const char *path);

/** Whether every member of object is a string, the shape of an id-to-version map. */
int runtime_is_version_map(const cJSON *value);

#endif
