#ifndef RUNTIME_CATALOG_H
#define RUNTIME_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#include "export.h"

/**
 * catalog.read: {text, origin} answers {catalog}, the index checked against catalog.schema.json and the
 * rules no schema can say, every file url resolved against origin; an index it refuses is refused with a
 * message naming the JSON pointer at fault.
 */
RUNTIME_EXPORT int32_t runtime_catalog_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** catalog.sources: {settings} answers {sources, skipped}, from the sources settings name under catalog.sources. */
RUNTIME_EXPORT int32_t runtime_catalog_sources(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** catalog.merge: {catalogs: [{source, catalog}]} answers {catalog, shadowed}, an earlier source winning an id. */
RUNTIME_EXPORT int32_t runtime_catalog_merge(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * catalog.check: {source, cache, now, intervalHours} answers {kind: skipped | unchanged | changed, catalog
 * | failed, detail}, asking an https source at most once per interval, conditionally on the etag kept
 * under cache/catalog, and reading a local source from disk every time.
 *
 * @implNote A failed check never moves checkedAt, so the next start asks again.
 */
RUNTIME_EXPORT int32_t runtime_catalog_check(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/** catalog.current: {sources, cache} answers {catalog}, every source's current index merged, with no request. */
RUNTIME_EXPORT int32_t runtime_catalog_current(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

/**
 * catalog.resolve: {catalog, enabled, installed, kind?} answers {run, fetch}: which installed version each
 * enabled item of kind runs this start, and which versions to fetch for the next.
 *
 * @implNote Only an installed version is ever chosen to run, so a fetch that fails or is still under way
 * never changes what runs. An unpinned item runs the catalog's latest when it is installed, over a newer
 * installed version, so a publisher can roll a broken release back.
 */
RUNTIME_EXPORT int32_t runtime_catalog_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
