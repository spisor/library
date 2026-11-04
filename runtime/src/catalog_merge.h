#ifndef RUNTIME_CATALOG_MERGE_H
#define RUNTIME_CATALOG_MERGE_H

#include <stddef.h>

#include "cJSON.h"
#include "catalog_source.h"

/** One source's catalog, as read. */
typedef struct runtime_catalog_reading {
    runtime_catalog_source source;
    const cJSON *catalog;
} runtime_catalog_reading;

/**
 * {format: 1, items} holding a copy of each id's item from the earliest reading listing it. Each later
 * reading's item of an id already held is added to shadowed as {id, source}, when shadowed is not NULL.
 * NULL when memory runs out.
 */
cJSON *runtime_catalog_merge_readings(const runtime_catalog_reading *readings, size_t count, cJSON *shadowed);

#endif
