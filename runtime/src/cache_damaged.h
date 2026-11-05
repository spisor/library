#ifndef RUNTIME_CACHE_DAMAGED_H
#define RUNTIME_CACHE_DAMAGED_H

#include "cJSON.h"

/**
 * Every installed bundle version, and every library version for platform, that fails the size check a
 * start makes or the full hash check, as [{id, version, detail, fix, file?}] in the order installed lists
 * them. NULL when memory runs out.
 *
 * @implNote The size check runs first so a marker that records no sizes, which a start refuses, is
 * reported as what the start refuses rather than passing on its hashes.
 */
cJSON *runtime_cache_damaged_versions(const char *cache, const char *platform);

#endif
