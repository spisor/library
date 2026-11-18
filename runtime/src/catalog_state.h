#ifndef RUNTIME_CATALOG_STATE_H
#define RUNTIME_CATALOG_STATE_H

#include "catalog_source.h"
#include "spisor/status.h"

/** What the cache keeps about one catalog source between starts; any member may be NULL. */
typedef struct runtime_catalog_state {
    char *etag;
    char *checked_at;
    char *index;
} runtime_catalog_state;

/**
 * Where the cache keeps one source's state: cache/catalog/<the SHA-256 of location>.json, location being
 * the resolved path of a local source or the url of an https one. Freed.
 */
char *runtime_catalog_state_path(const char *cache, const char *location);

/** The source's path resolved, or its url; freed, NULL when memory runs out. */
char *runtime_catalog_location_of(const runtime_catalog_location *source);

/**
 * The url the source's file urls resolve against, in *origin, freed: SPISOR_MALFORMED when it has none,
 * SPISOR_OUT_OF_MEMORY when memory runs out, *origin NULL for both.
 */
spisor_status runtime_catalog_origin_of(const runtime_catalog_location *source, const char *location, char **origin);

/**
 * The state kept at path; a missing or unreadable file, or one that is not an object, keeps nothing. 0 with state
 * empty when memory runs out, 1 otherwise.
 */
int runtime_catalog_read_state(const char *path, runtime_catalog_state *state);

/**
 * Writes state as JSON.stringify(state, null, 2) and a newline, replacing the file at once: OK; REFUSED when the file
 * could not be written; OUT_OF_MEMORY.
 */
spisor_status runtime_catalog_write_state(const char *path, const runtime_catalog_state *state);

/** The state a check decided to keep for the source at location, under path; every member NULL when it keeps none. */
typedef struct runtime_catalog_kept_state {
    char *path;
    char *location;
    runtime_catalog_state state;
} runtime_catalog_kept_state;

/** Writes kept when it holds a state, as runtime_catalog_write_state answers; OK when it holds none. */
spisor_status runtime_catalog_keep_state(const runtime_catalog_kept_state *kept);

void runtime_catalog_free_kept_state(runtime_catalog_kept_state *kept);

void runtime_catalog_free_state(runtime_catalog_state *state);

#endif
