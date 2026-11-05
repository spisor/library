#ifndef RUNTIME_VERSION_MAP_H
#define RUNTIME_VERSION_MAP_H

#include "cJSON.h"

/** The version map names for id, or NULL. */
const char *runtime_map_version(const cJSON *map, const char *id);

/** Sets map[id] to version as a JavaScript assignment does, an existing id keeping its place. 1 on success. */
int runtime_map_set(cJSON *map, const char *id, const char *version);

/** Every string entry of from set into into, as {...into, ...from} orders them. 1 on success. */
int runtime_map_merge(cJSON *into, const cJSON *from);

/** Whether the bundle version is installed in cache with every file still carrying its recorded size and hash. */
int runtime_bundle_is_intact(const char *cache, const char *id, const char *version);

#endif
