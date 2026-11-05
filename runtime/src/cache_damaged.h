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

/**
 * The {detail, fix, file?} of the version in directory when the size check or the hash check finds it
 * damaged; NULL when it is intact or gone, *out_of_memory set when memory ran out.
 *
 * @implNote Gone covers a sweep removing the marker between the installed listing and this check: that
 * version is not installed any more, which is no damage to report.
 */
cJSON *runtime_cache_version_damage(const char *directory, int *out_of_memory);

#endif
