#ifndef RUNTIME_PUBLIC_CACHE_H
#define RUNTIME_PUBLIC_CACHE_H

/**
 * @file
 * runtime's `cache` service: the bundle and library versions installed in the cache place, each in a folder of its
 * own, cache/bundles/<id>/<version>/ or cache/libraries/<id>/<version>/<platform>/, complete once its marker
 * .installed.json is written. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the release
 * for its kind. A place or a folder is a directory's path, and a platform is a key such as linux-x64, this
 * machine's for NULL.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Version `version` of the catalog item `id`, whose `item` is the record catalog's read answers for it, stored in the
 * `cache` place for `platform`. First shape ends at `platform`.
 */
typedef struct runtime_cache_install_argument {
    uint32_t size;
    const char *cache;
    const char *id;
    const spisor_value *item;
    const char *version;
    const char *platform;
} runtime_cache_install_argument;

/**
 * The version installed in `directory`, with the keys of the files it `fetched` and of those it found already
 * `cached` with their hash. First shape ends at `storage`.
 */
typedef struct runtime_cache_install_answer {
    uint32_t size;
    const char *id;
    const char *version;
    const char *directory;
    const char *const *fetched;
    size_t fetched_count;
    const char *const *cached;
    size_t cached_count;
    void *storage;
} runtime_cache_install_answer;

/** The `cache` place, read for `platform`'s libraries. First shape ends at `platform`. */
typedef struct runtime_cache_place_argument {
    uint32_t size;
    const char *cache;
    const char *platform;
} runtime_cache_place_argument;

/** One id's installed versions, in the order the cache lists them. An element of an array, so it has no size. */
typedef struct runtime_cache_versions {
    const char *id;
    const char *const *versions;
    size_t version_count;
} runtime_cache_versions;

/** Every id with a version installed, bundles and libraries each in id order. First shape ends at `storage`. */
typedef struct runtime_cache_installed_answer {
    uint32_t size;
    const runtime_cache_versions *bundles;
    size_t bundle_count;
    const runtime_cache_versions *libraries;
    size_t library_count;
    void *storage;
} runtime_cache_installed_answer;

/** An installed version's folder, as install answers it. First shape ends at `directory`. */
typedef struct runtime_cache_check_argument {
    uint32_t size;
    const char *directory;
} runtime_cache_check_argument;

/** What check found. Append-only: a published value keeps its number. */
typedef enum runtime_cache_verdict {
    RUNTIME_CACHE_ABSENT = 1,
    RUNTIME_CACHE_INTACT = 2,
    RUNTIME_CACHE_DAMAGED = 3
} runtime_cache_verdict;

/**
 * Its runtime_cache_verdict, and for a damaged version what is wrong, what repairs it and the file at fault, NULL when
 * no one file is. First shape ends at `storage`.
 */
typedef struct runtime_cache_check_answer {
    uint32_t size;
    uint32_t verdict;
    const char *detail;
    const char *fix;
    const char *file;
    void *storage;
} runtime_cache_check_answer;

/**
 * A version of `id` that a start would refuse or whose files lost their hashes: what is wrong, what repairs it, and
 * the file at fault, NULL when no one file is. An element of an array, so it has no size.
 */
typedef struct runtime_cache_damage {
    const char *id;
    const char *version;
    const char *detail;
    const char *fix;
    const char *file;
} runtime_cache_damage;

/** Every damaged version, in the order installed lists them. First shape ends at `storage`. */
typedef struct runtime_cache_damaged_answer {
    uint32_t size;
    const runtime_cache_damage *damaged;
    size_t damaged_count;
    void *storage;
} runtime_cache_damaged_answer;

/** A plugin id and the version it runs. An element of an array, so it has no size. */
typedef struct runtime_cache_plugin {
    const char *id;
    const char *version;
} runtime_cache_plugin;

/** The plugins to find in the `cache` place, each id once. First shape ends at `plugin_count`. */
typedef struct runtime_cache_locate_argument {
    uint32_t size;
    const char *cache;
    const runtime_cache_plugin *plugins;
    size_t plugin_count;
} runtime_cache_locate_argument;

/**
 * Where a plugin's version is installed: its folder, its plugin.json, and the entry that names, NULL when it names
 * none that is there. An element of an array, so it has no size.
 */
typedef struct runtime_cache_located {
    const char *id;
    const char *version;
    const char *directory;
    const char *manifest;
    const char *entry;
} runtime_cache_located;

/**
 * The plugins found, and those whose installed version cannot run, each in id order; a version not installed is in
 * neither. First shape ends at `storage`.
 */
typedef struct runtime_cache_locate_answer {
    uint32_t size;
    const runtime_cache_located *located;
    size_t located_count;
    const runtime_cache_damage *problems;
    size_t problem_count;
    void *storage;
} runtime_cache_locate_answer;

/** This build's platform key as Node spells it, process.platform-process.arch. First shape ends at `storage`. */
typedef struct runtime_cache_platform_answer {
    uint32_t size;
    const char *platform;
    void *storage;
} runtime_cache_platform_answer;

/** The `cache` place to sweep and the `state` place whose references keep a version. First shape ends at `state`. */
typedef struct runtime_cache_sweep_argument {
    uint32_t size;
    const char *cache;
    const char *state;
} runtime_cache_sweep_argument;

/**
 * Each removed version as id@version, a library's as id@version for platform: bundles first, then libraries. First
 * shape ends at `storage`.
 */
typedef struct runtime_cache_sweep_answer {
    uint32_t size;
    const char *const *removed;
    size_t removed_count;
    void *storage;
} runtime_cache_sweep_answer;

/**
 * The plugin installed in `directory`, put where a host loads it from in `plugin_directory`, as the plugin `id` named
 * `name`; `replaces`, or NULL, an id whose deployed plugin of the same import name this one may replace. First shape
 * ends at `replaces`.
 */
typedef struct runtime_cache_deploy_argument {
    uint32_t size;
    const char *directory;
    const char *plugin_directory;
    const char *id;
    const char *name;
    const char *replaces;
} runtime_cache_deploy_argument;

/** The import name the plugin was deployed under. First shape ends at `storage`. */
typedef struct runtime_cache_deploy_answer {
    uint32_t size;
    const char *import_name;
    void *storage;
} runtime_cache_deploy_answer;

/**
 * The cache service. Every operation answers MALFORMED for an argument short of its first shape, a place or folder
 * that is empty, or a required text that is NULL or empty; and OUT_OF_MEMORY. The answer arguments are required and
 * the refusal may be NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_cache_definition {
    uint32_t size;
    void *context;
    /**
     * Stores every file the item's version lists for the platform, fetching only what is not already stored with its
     * hash, and writes the marker last.
     *
     * @return OK; MALFORMED, also for an item that is not a well-formed record or an empty platform; REFUSED when the
     * item does not list the version, a file cannot be fetched or does not carry its hash, a destination escapes the
     * cache, or the marker cannot be written; OUT_OF_MEMORY. A refused
     * or OUT_OF_MEMORY install leaves no marker, so nothing runs the version, and may leave files it stored, which the
     * next install of it keeps.
     */
    spisor_status (*install)(void *context, const runtime_cache_install_argument *argument,
                             runtime_cache_install_answer *answer, spisor_refusal *refusal);
    /**
     * Each id's versions whose install completed and whose recorded files are present, read from the folders and
     * markers without hashing.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*installed)(void *context, const runtime_cache_place_argument *argument,
                               runtime_cache_installed_answer *answer, spisor_refusal *refusal);
    /**
     * Whether the version in the folder is complete with every file at its recorded hash, local and network-free.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*check)(void *context, const runtime_cache_check_argument *argument,
                           runtime_cache_check_answer *answer, spisor_refusal *refusal);
    /**
     * Every installed version a start would refuse on its sizes or whose files no longer carry their hashes. It hashes
     * every installed file, so status runs it and a start does not.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY.
     */
    spisor_status (*damaged)(void *context, const runtime_cache_place_argument *argument,
                             runtime_cache_damaged_answer *answer, spisor_refusal *refusal);
    /**
     * Where each plugin's version is installed with its plugin.json and entry, or what stops it running, checking
     * sizes only, so a start can afford it.
     *
     * @return OK; MALFORMED, also for a plugin without an id or a version, or an id given twice; OUT_OF_MEMORY.
     */
    spisor_status (*locate)(void *context, const runtime_cache_locate_argument *argument,
                            runtime_cache_locate_answer *answer, spisor_refusal *refusal);
    /**
     * This build's platform key.
     *
     * @return OK; MALFORMED for an answer short of its first shape; OUT_OF_MEMORY.
     */
    spisor_status (*platform)(void *context, runtime_cache_platform_answer *answer, spisor_refusal *refusal);
    /**
     * Deletes every version no reference, live run record or boot hint under the places names, keeping each id's
     * newest installed version, per platform for a library, as the rollback. Holds the registry lock for its whole
     * call. It reads every reference and decides every removal before it removes anything, so a sweep that runs out
     * of memory reading a reference removes nothing; one that runs out later may have removed some versions, each of
     * them one the sweep was removing, never a referenced one or a kept rollback. A version that cannot be removed is
     * skipped for the next sweep.
     *
     * @return OK; MALFORMED; REFUSED when the registry lock cannot be taken; OUT_OF_MEMORY.
     */
    spisor_status (*sweep)(void *context, const runtime_cache_sweep_argument *argument,
                           runtime_cache_sweep_answer *answer, spisor_refusal *refusal);
    /**
     * Writes the plugin's plugin.json, its entry when it names one, and its identity into the plugin folder, under the
     * import name its own plugin.json gives, everything checked before the first file is written.
     *
     * @return OK; MALFORMED, also for an id that is not an identity id; REFUSED when the folder holds no installed
     * plugin, its import name is not one the schema accepts, another plugin is deployed under that name, or a file
     * cannot be written; OUT_OF_MEMORY, having written nothing.
     */
    spisor_status (*deploy)(void *context, const runtime_cache_deploy_argument *argument,
                            runtime_cache_deploy_answer *answer, spisor_refusal *refusal);
    void (*release_install_answer)(void *context, runtime_cache_install_answer *answer);
    void (*release_installed_answer)(void *context, runtime_cache_installed_answer *answer);
    void (*release_check_answer)(void *context, runtime_cache_check_answer *answer);
    void (*release_damaged_answer)(void *context, runtime_cache_damaged_answer *answer);
    void (*release_locate_answer)(void *context, runtime_cache_locate_answer *answer);
    void (*release_platform_answer)(void *context, runtime_cache_platform_answer *answer);
    void (*release_sweep_answer)(void *context, runtime_cache_sweep_answer *answer);
    void (*release_deploy_answer)(void *context, runtime_cache_deploy_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_cache_definition;

/** The static runtime_cache_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_cache_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
