#ifndef RUNTIME_PUBLIC_CATALOG_H
#define RUNTIME_PUBLIC_CATALOG_H

/**
 * @file
 * runtime's `catalog` service: the catalog indexes that list what can be installed, read from the sources settings
 * name, merged, kept current under the cache place, and resolved into the versions each start runs. A catalog crosses
 * as the index's record, `format`, `items` (every item by id) and any member a later format adds, whose shape the
 * catalog's publisher decides. A size-prefixed struct
 * is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the call; what an
 * answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/item_version.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where a source's index is read from. Append-only: a published value keeps its number. */
typedef enum runtime_catalog_source_kind {
    RUNTIME_CATALOG_LOCAL = 1,
    RUNTIME_CATALOG_HTTPS = 2
} runtime_catalog_source_kind;

/**
 * A catalog source: a runtime_catalog_source_kind and its location, a local index file's path or an https index's url.
 * An element of an array, so it has no size.
 */
typedef struct runtime_catalog_source {
    uint32_t kind;
    const char *location;
} runtime_catalog_source;

/** An index's `text` and the url it was read from, every file url in it resolved against. First shape ends at `origin`. */
typedef struct runtime_catalog_read_argument {
    uint32_t size;
    const char *text;
    const char *origin;
} runtime_catalog_read_argument;

/** A catalog's record. First shape ends at `storage`. */
typedef struct runtime_catalog_answer {
    uint32_t size;
    const spisor_value *catalog;
    void *storage;
} runtime_catalog_answer;

/** The merged settings, whose `catalog.sources` lists the sources. First shape ends at `settings`. */
typedef struct runtime_catalog_sources_argument {
    uint32_t size;
    const spisor_value *settings;
} runtime_catalog_sources_argument;

/** A listed source that is not one: its position in the list and why. An element of an array, so it has no size. */
typedef struct runtime_catalog_skipped {
    size_t index;
    const char *detail;
} runtime_catalog_skipped;

/** The sources in their order, and the entries skipped. First shape ends at `storage`. */
typedef struct runtime_catalog_sources_answer {
    uint32_t size;
    const runtime_catalog_source *sources;
    size_t source_count;
    const runtime_catalog_skipped *skipped;
    size_t skipped_count;
    void *storage;
} runtime_catalog_sources_answer;

/** A source and the catalog record its index holds. An element of an array, so it has no size. */
typedef struct runtime_catalog_reading {
    runtime_catalog_source source;
    const spisor_value *catalog;
} runtime_catalog_reading;

/** The catalogs to merge, an earlier one winning an id. First shape ends at `catalog_count`. */
typedef struct runtime_catalog_merge_argument {
    uint32_t size;
    const runtime_catalog_reading *catalogs;
    size_t catalog_count;
} runtime_catalog_merge_argument;

/** An item a later source also lists, and that source. An element of an array, so it has no size. */
typedef struct runtime_catalog_shadowed {
    const char *id;
    runtime_catalog_source source;
} runtime_catalog_shadowed;

/** The merged catalog and every item an earlier source shadowed. First shape ends at `storage`. */
typedef struct runtime_catalog_merge_answer {
    uint32_t size;
    const spisor_value *catalog;
    const runtime_catalog_shadowed *shadowed;
    size_t shadowed_count;
    void *storage;
} runtime_catalog_merge_answer;

/**
 * A source to check, the absolute `cache` place its state is kept under, `now` as an ISO 8601 time such as
 * toISOString writes, and how many hours an https source goes unasked. First shape ends at `interval_hours`.
 */
typedef struct runtime_catalog_check_argument {
    uint32_t size;
    const runtime_catalog_source *source;
    const char *cache;
    const char *now;
    double interval_hours;
} runtime_catalog_check_argument;

/** What a check found. Append-only: a published value keeps its number. */
typedef enum runtime_catalog_check_kind {
    RUNTIME_CATALOG_SKIPPED = 1,
    RUNTIME_CATALOG_UNCHANGED = 2,
    RUNTIME_CATALOG_CHANGED = 3,
    RUNTIME_CATALOG_FAILED = 4
} runtime_catalog_check_kind;

/**
 * Its runtime_catalog_check_kind, the changed index's catalog record, NULL for any other kind, and for a failed check
 * what failed. First shape ends at `storage`.
 */
typedef struct runtime_catalog_check_answer {
    uint32_t size;
    uint32_t kind;
    const spisor_value *catalog;
    const char *detail;
    void *storage;
} runtime_catalog_check_answer;

/** The sources whose current indexes to merge, kept under the absolute `cache` place. First shape ends at `cache`. */
typedef struct runtime_catalog_current_argument {
    uint32_t size;
    const runtime_catalog_source *sources;
    size_t source_count;
    const char *cache;
} runtime_catalog_current_argument;

/** An enabled item and the version it is pinned to, or NULL. An element of an array, so it has no size. */
typedef struct runtime_catalog_enabled {
    const char *id;
    const char *pin;
} runtime_catalog_enabled;

/** One id's installed versions. An element of an array, so it has no size. */
typedef struct runtime_catalog_installed {
    const char *id;
    const char *const *versions;
    size_t version_count;
} runtime_catalog_installed;

/** Item kinds resolve reads. Append-only: a published value keeps its number. */
typedef enum runtime_catalog_item_kind {
    RUNTIME_CATALOG_PLUGIN = 1,
    RUNTIME_CATALOG_HOST = 2,
    RUNTIME_CATALOG_LIBRARY = 3
} runtime_catalog_item_kind;

/**
 * The catalog's record, the enabled items in order, each id's installed versions, each id once, and the
 * runtime_catalog_item_kind to resolve. First shape ends at `kind`.
 */
typedef struct runtime_catalog_resolve_argument {
    uint32_t size;
    const spisor_value *catalog;
    const runtime_catalog_enabled *enabled;
    size_t enabled_count;
    const runtime_catalog_installed *installed;
    size_t installed_count;
    uint32_t kind;
} runtime_catalog_resolve_argument;

/** The installed version each enabled item runs this start, and the versions to fetch for the next. First shape ends at `storage`. */
typedef struct runtime_catalog_resolve_answer {
    uint32_t size;
    const runtime_item_version *run;
    size_t run_count;
    const runtime_item_version *fetch;
    size_t fetch_count;
    void *storage;
} runtime_catalog_resolve_answer;

/**
 * The catalog service. Every operation answers MALFORMED for an argument or answer short of its first shape, a
 * required member that is NULL or empty, a source of no known kind or without its location, an argument count
 * without its array, or a catalog that is not a well-formed record
 * with an `items` record; and OUT_OF_MEMORY. The answer arguments are required
 * and the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_catalog_definition {
    uint32_t size;
    void *context;
    /**
     * The index checked against catalog.schema.json and the rules no schema can say.
     *
     * @return OK; MALFORMED; REFUSED for an index it refuses, the refusal naming the JSON pointer at fault.
     */
    spisor_status (*read)(void *context, const runtime_catalog_read_argument *argument, runtime_catalog_answer *answer,
                          spisor_refusal *refusal);
    /**
     * The sources the settings list under catalog.sources, skipping each entry that is not one.
     *
     * @return OK; MALFORMED, also for settings that are not a record; OUT_OF_MEMORY.
     */
    spisor_status (*sources)(void *context, const runtime_catalog_sources_argument *argument,
                             runtime_catalog_sources_answer *answer, spisor_refusal *refusal);
    /**
     * The catalogs merged, an earlier one winning an id.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*merge)(void *context, const runtime_catalog_merge_argument *argument,
                           runtime_catalog_merge_answer *answer, spisor_refusal *refusal);
    /**
     * Asks an https source at most once per interval, conditionally on the etag kept under the cache, and reads a
     * local source from disk every time. A failed check never moves the time it was last checked, so the next start
     * asks again; it answers OK with the kind FAILED.
     *
     * @return OK; MALFORMED, also for a cache that is not absolute or a time that is not ISO 8601; OUT_OF_MEMORY.
     */
    spisor_status (*check)(void *context, const runtime_catalog_check_argument *argument,
                           runtime_catalog_check_answer *answer, spisor_refusal *refusal);
    /**
     * Every source's index as it stands, merged, with no request: a local index read from disk, an https one as last
     * kept.
     *
     * @return OK; MALFORMED, also for a cache that is not absolute; OUT_OF_MEMORY.
     */
    spisor_status (*current)(void *context, const runtime_catalog_current_argument *argument,
                             runtime_catalog_answer *answer, spisor_refusal *refusal);
    /**
     * Which installed version each enabled item of the kind runs this start, and which versions to fetch for the
     * next. Only an installed version is ever chosen to run, and an unpinned item runs the catalog's latest when it is
     * installed, over a newer installed version, so a publisher can roll a broken release back.
     *
     * @return OK; MALFORMED, also for a kind it does not know or an id installed twice; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_catalog_resolve_argument *argument,
                             runtime_catalog_resolve_answer *answer, spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_catalog_answer *answer);
    void (*release_sources_answer)(void *context, runtime_catalog_sources_answer *answer);
    void (*release_merge_answer)(void *context, runtime_catalog_merge_answer *answer);
    void (*release_check_answer)(void *context, runtime_catalog_check_answer *answer);
    void (*release_resolve_answer)(void *context, runtime_catalog_resolve_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_catalog_definition;

/** The static runtime_catalog_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_catalog_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
