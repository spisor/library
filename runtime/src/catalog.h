#ifndef RUNTIME_CATALOG_H
#define RUNTIME_CATALOG_H

#include "cJSON.h"
#include "catalog_state.h"

/** The format of a catalog index, its `format` member, as catalog.schema.json pins it. */
#define RUNTIME_CATALOG_FORMAT 1

/**
 * @file
 * The catalog service's functions, for runtime's own callers: each answers its result, or NULL with *refusal set to
 * {message, fix}, or with it NULL when memory runs out.
 */

/**
 * catalog's read: the index text, read from origin, as {catalog}, the index checked against catalog.schema.json and
 * the rules no schema can say, every file url resolved against origin; an index it refuses is refused with a
 * message naming the JSON pointer at fault.
 */
cJSON *runtime_catalog_read(const char *text, const char *origin, cJSON **refusal);

/**
 * catalog's sources: {sources, skipped}, from the sources `settings`, the merged settings object, name under
 * catalog.sources; NULL when memory runs out.
 */
cJSON *runtime_catalog_sources(const cJSON *settings);

/** catalog's merge: catalogs, [{source, catalog}], as {catalog, shadowed}, an earlier source winning an id. */
cJSON *runtime_catalog_merge(const cJSON *catalogs, cJSON **refusal);

/** One check: the source as catalog's sources answers it, the cache place, the time now, and the update interval. */
typedef struct runtime_catalog_check_request {
    const cJSON *source;
    const char *cache;
    const char *now;
    double interval_hours;
} runtime_catalog_check_request;

/**
 * catalog's check: {kind: skipped | unchanged | changed, catalog | failed, detail}, asking an https source at most
 * once per interval, conditionally on the etag kept under cache/catalog, and reading a local source from disk every
 * time; or NULL with *refusal set, or with it NULL when memory runs out.
 *
 * @implNote A failed check never moves checkedAt, so the next start asks again.
 */
cJSON *runtime_catalog_check(const runtime_catalog_check_request *request, cJSON **refusal);

/**
 * runtime_catalog_check without its write: the state the check keeps goes to *kept, empty when it keeps none or the
 * answer is NULL, for the caller to write with runtime_catalog_keep_state once its own answer stands.
 */
cJSON *runtime_catalog_check_deciding(const runtime_catalog_check_request *request, cJSON **refusal,
                                      runtime_catalog_kept_state *kept);

/** The failed answer of a check whose kept state could not be written; NULL out of memory. */
cJSON *runtime_catalog_unkept_answer(const runtime_catalog_kept_state *kept);

/**
 * catalog's current: the catalog of every one of `sources`, as catalog's sources answers them, merged from its
 * current index under `cache`, with no request; or NULL with *refusal set, or with it NULL when memory runs out.
 */
cJSON *runtime_catalog_current(const cJSON *sources, const char *cache, cJSON **refusal);

/**
 * catalog's resolve: {run, fetch}, which installed version each of `enabled`, [{id, pin?}], of `kind` (plugin when
 * NULL) runs this start, given each id's `installed` versions, and which versions to fetch for the next; or NULL with
 * *refusal set, or with it NULL when memory runs out.
 *
 * @implNote Only an installed version is ever chosen to run, so a fetch that fails or is still under way
 * never changes what runs. An unpinned item runs the catalog's latest when it is installed, over a newer
 * installed version, so a publisher can roll a broken release back.
 */
cJSON *runtime_catalog_resolve(const cJSON *catalog, const cJSON *enabled, const cJSON *installed, const char *kind,
                               cJSON **refusal);

#endif
