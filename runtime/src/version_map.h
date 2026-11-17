#ifndef RUNTIME_VERSION_MAP_H
#define RUNTIME_VERSION_MAP_H

#include "cJSON.h"

/** The version map names for id, or NULL. */
const char *runtime_map_version(const cJSON *map, const char *id);

/** Sets map[id] to version as a JavaScript assignment does, an existing id keeping its place. 1 on success. */
int runtime_map_set(cJSON *map, const char *id, const char *version);

/** Every string entry of from set into into, as {...into, ...from} orders them. 1 on success. */
int runtime_map_merge(cJSON *into, const cJSON *from);

/**
 * Whether the bundle version is installed in cache with every file still carrying its recorded size and hash; 0 with
 * *out_of_memory set to 1 when memory ran out finding out, and to 0 otherwise.
 *
 * @implNote Hashes every file of the version, so it never runs under the registry lock or on a start path;
 * those check runtime_bundle_is_sized.
 */
int runtime_bundle_is_intact(const char *cache, const char *id, const char *version, int *out_of_memory);

/**
 * Whether the bundle version is installed in cache with every file present at its recorded size: the check
 * a start makes, and the one repeated under the registry lock after a full check made before it. *out_of_memory as
 * runtime_bundle_is_intact sets it.
 */
int runtime_bundle_is_sized(const char *cache, const char *id, const char *version, int *out_of_memory);

#endif
