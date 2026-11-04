#ifndef RUNTIME_CATALOG_SOURCE_H
#define RUNTIME_CATALOG_SOURCE_H

#include <stddef.h>

#include "cJSON.h"

/** A catalog source: a local index file, by path, or an https index, by url. */
typedef struct runtime_catalog_source {
    int local;
    const char *location;
} runtime_catalog_source;

/**
 * Reads entry as a source, borrowing its strings. 1 on success; otherwise 0 and *problem, freed with free,
 * says why, or is NULL when memory ran out.
 */
int runtime_catalog_source_of(const cJSON *entry, runtime_catalog_source *source, char **problem);

/** {kind: "local", path} or {kind: "https", url}; NULL when memory runs out. */
cJSON *runtime_catalog_source_json(const runtime_catalog_source *source);

/**
 * Reads every entry of array as a source, into sources, which holds one per entry and is freed with free.
 * 0 when an entry is no source, naming it in *problem, or when memory runs out, *problem being NULL.
 */
int runtime_catalog_sources_of(const cJSON *array, runtime_catalog_source **sources, size_t *count, char **problem);

#endif
