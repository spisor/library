#ifndef RUNTIME_CACHE_SWEEP_H
#define RUNTIME_CACHE_SWEEP_H

#include "cJSON.h"

/**
 * Deletes every version runtime_cache_referenced does not name, keeping each id's newest installed
 * version (per platform for a library) as the rollback, and answers the removed ones as id@version, a
 * library's as id@version for platform: bundles first, then libraries. NULL when memory runs out.
 *
 * @implNote The caller holds the registry lock. A version with no completion marker counts only once
 * nothing in it changed for a day: a download is bounded by fetch's own deadline, so a day is far past
 * any install still writing, while short enough that a failed one stops holding its disk.
 */
cJSON *runtime_cache_sweep_versions(const char *cache, const char *state);

#endif
