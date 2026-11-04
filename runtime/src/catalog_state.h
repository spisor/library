#ifndef RUNTIME_CATALOG_STATE_H
#define RUNTIME_CATALOG_STATE_H

#include "catalog_source.h"

/** What the cache keeps about one catalog source between starts; any member may be NULL. */
typedef struct runtime_catalog_state {
    char *etag;
    char *checked_at;
    char *index;
} runtime_catalog_state;

/**
 * Where the cache keeps one source's state: cache/catalog/<the SHA-256 of location>.json, location being
 * the resolved path of a local source or the url of an https one. Freed with free.
 */
char *runtime_catalog_state_path(const char *cache, const char *location);

/** The source's path resolved, or its url; freed with free, NULL when memory runs out. */
char *runtime_catalog_location_of(const runtime_catalog_source *source);

/** The url the source's file urls resolve against; freed with free, NULL when it has none. */
char *runtime_catalog_origin_of(const runtime_catalog_source *source, const char *location);

/** The state kept at path; a missing or unreadable file, or one that is not an object, keeps nothing. */
void runtime_catalog_read_state(const char *path, runtime_catalog_state *state);

/** Writes state as JSON.stringify(state, null, 2) and a newline, replacing the file at once. 1 on success. */
int runtime_catalog_write_state(const char *path, const runtime_catalog_state *state);

void runtime_catalog_free_state(runtime_catalog_state *state);

#endif
