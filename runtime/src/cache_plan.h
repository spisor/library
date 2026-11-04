#ifndef RUNTIME_CACHE_PLAN_H
#define RUNTIME_CACHE_PLAN_H

#include <stddef.h>

#include "cJSON.h"

typedef struct runtime_cache_placement {
    const char *key;
    const cJSON *reference;
    char *destination;
} runtime_cache_placement;

/** Where every file of one version goes, in the order JavaScript enumerates the merged file keys. */
typedef struct runtime_cache_plan {
    char *directory;
    runtime_cache_placement *placements;
    size_t count;
} runtime_cache_plan;

/**
 * Plans installing version of item id for platform: the version directory and each file's destination,
 * every one checked before anything is written. 1 with plan filled; 0 with *refusal set to its
 * {message, fix}, or with *refusal NULL when memory runs out. Release with runtime_cache_free_plan.
 */
int runtime_cache_plan_install(const char *cache, const char *id, const cJSON *item, const char *version,
                               const char *platform, runtime_cache_plan *plan, cJSON **refusal);

void runtime_cache_free_plan(runtime_cache_plan *plan);

#endif
