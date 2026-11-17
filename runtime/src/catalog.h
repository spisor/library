#ifndef RUNTIME_CATALOG_H
#define RUNTIME_CATALOG_H

#include "cJSON.h"
#include "catalog_state.h"

/**
 * @file
 * The catalog service's operations over JSON, for runtime's own callers: each answers its result, or NULL with *refusal
 * set to {message, fix}, or with it NULL when memory runs out.
 */

/**
 * catalog's read: {text, origin} answers {catalog}, the index checked against catalog.schema.json and the
 * rules no schema can say, every file url resolved against origin; an index it refuses is refused with a
 * message naming the JSON pointer at fault.
 */
cJSON *runtime_catalog_read_operation(const cJSON *argument, cJSON **refusal);

/** catalog's sources: {settings} answers {sources, skipped}, from the sources settings name under catalog.sources. */
cJSON *runtime_catalog_sources_operation(const cJSON *argument, cJSON **refusal);

/** catalog's merge: {catalogs: [{source, catalog}]} answers {catalog, shadowed}, an earlier source winning an id. */
cJSON *runtime_catalog_merge_operation(const cJSON *argument, cJSON **refusal);

/**
 * catalog's check: {source, cache, now, intervalHours} answers {kind: skipped | unchanged | changed, catalog
 * | failed, detail}, asking an https source at most once per interval, conditionally on the etag kept
 * under cache/catalog, and reading a local source from disk every time.
 *
 * @implNote A failed check never moves checkedAt, so the next start asks again.
 */
cJSON *runtime_catalog_check_operation(const cJSON *argument, cJSON **refusal);

/**
 * runtime_catalog_check_operation without its write: the state the check keeps goes to *kept, empty when it keeps
 * none or the answer is NULL, for the caller to write with runtime_catalog_keep_state once its own answer stands.
 */
cJSON *runtime_catalog_check_deciding(const cJSON *argument, cJSON **refusal, runtime_catalog_kept_state *kept);

/** The failed answer of a check whose kept state could not be written; NULL out of memory. */
cJSON *runtime_catalog_unkept_answer(const runtime_catalog_kept_state *kept);

/** catalog's current: {sources, cache} answers {catalog}, every source's current index merged, with no request. */
cJSON *runtime_catalog_current_operation(const cJSON *argument, cJSON **refusal);

/**
 * catalog's resolve: {catalog, enabled, installed, kind?} answers {run, fetch}: which installed version each
 * enabled item of kind runs this start, and which versions to fetch for the next.
 *
 * @implNote Only an installed version is ever chosen to run, so a fetch that fails or is still under way
 * never changes what runs. An unpinned item runs the catalog's latest when it is installed, over a newer
 * installed version, so a publisher can roll a broken release back.
 */
cJSON *runtime_catalog_resolve_operation(const cJSON *argument, cJSON **refusal);

#endif
