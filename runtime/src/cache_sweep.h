#ifndef RUNTIME_CACHE_SWEEP_H
#define RUNTIME_CACHE_SWEEP_H

#include "cJSON.h"

/**
 * Deletes every version runtime_cache_referenced does not name at now, keeping each id's newest installed
 * version (per platform for a library) as the rollback, and answers the removed ones as id@version, a
 * library's as id@version for platform: bundles first, then libraries. NULL when memory runs out, having removed
 * nothing when it ran out while judging which versions to remove. It also removes each file directly inside the state
 * place's results folder untouched for a day before now, which it does not answer.
 *
 * @implNote The caller holds the registry lock. A version with no completion marker counts only once
 * nothing in it changed for a day before now: a download is bounded by fetch's own deadline, so a day is far past
 * any install still writing, while short enough that a failed one stops holding its disk. A result file is read
 * by its caller the moment its run exits, so one a day old is a killed caller's, which no run will read again.
 */
cJSON *runtime_cache_sweep_versions(const char *cache, const char *state, double now);

#endif
