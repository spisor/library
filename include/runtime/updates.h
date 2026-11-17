#ifndef RUNTIME_PUBLIC_UPDATES_H
#define RUNTIME_PUBLIC_UPDATES_H

/**
 * @file
 * runtime's `updates` service: which version of each host, plugin and library a host runs, schedules next and keeps
 * as rollback, recorded in state/bundles/<host id>.json under the registry lock that cache's sweep holds too. A
 * size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for
 * the call; what an answer or a refusal points at is the provider's until the release for its kind. A place is an
 * absolute directory, a host id a plain file name, and a platform a key such as linux-x64, this machine's for NULL.
 * The settings a host's settings service merged cross as spisor_value, their shape being that service's.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/install.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/** An id and its version, such as a plugin a start runs. An element of an array, so it has no size. */
typedef struct runtime_updates_version {
    const char *id;
    const char *version;
} runtime_updates_version;

/** A host default: the plugin a binding starts unless the settings say otherwise. An element of an array. */
typedef struct runtime_updates_default {
    const char *binding;
    const char *plugin;
} runtime_updates_default;

/** What could not be recorded, or was refused for `id`, and how to fix it. An element of an array. */
typedef struct runtime_updates_problem {
    const char *id;
    const char *detail;
    const char *fix;
} runtime_updates_problem;

/**
 * A start of `host_id` with its merged `settings`, a record or NULL; its host `defaults`; the other versions it runs
 * beside its plugins, such as the libraries it loaded; and the ids of the libraries it boots with, NULL to leave its
 * boot hint alone. First shape ends at `platform`.
 */
typedef struct runtime_updates_apply_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *host_id;
    const spisor_value *settings;
    const runtime_updates_default *defaults;
    size_t default_count;
    const runtime_updates_version *running;
    size_t running_count;
    const char *const *libraries;
    size_t library_count;
    const char *platform;
} runtime_updates_apply_argument;

/**
 * The plugins this start runs, what it records as run, next and rollback, what it could not record without that
 * stopping it (each problem's `id` NULL), and the path of this start's run record. First shape ends at `storage`.
 */
typedef struct runtime_updates_apply_answer {
    uint32_t size;
    const runtime_updates_version *plugins;
    size_t plugin_count;
    const runtime_updates_version *run;
    size_t run_count;
    const runtime_updates_version *next;
    size_t next_count;
    const runtime_updates_version *rollback;
    size_t rollback_count;
    const runtime_updates_problem *unrecorded;
    size_t unrecorded_count;
    const char *run_record;
    void *storage;
} runtime_updates_apply_answer;

/**
 * A check for `host_id` at `now`, an ISO time, with its merged `settings`, a record or NULL, its host `defaults`, and
 * the damaged versions to schedule again. First shape ends at `platform`.
 */
typedef struct runtime_updates_check_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *host_id;
    const char *now;
    const spisor_value *settings;
    const runtime_updates_default *defaults;
    size_t default_count;
    const runtime_updates_version *damaged;
    size_t damaged_count;
    const char *platform;
} runtime_updates_check_argument;

/** How one catalog source's check went. Append-only: a published value keeps its number. */
typedef enum runtime_updates_source_outcome {
    RUNTIME_UPDATES_SKIPPED = 1,
    RUNTIME_UPDATES_UNCHANGED = 2,
    RUNTIME_UPDATES_CHANGED = 3,
    RUNTIME_UPDATES_FAILED = 4
} runtime_updates_source_outcome;

/** A catalog source's runtime_updates_source_outcome and what it said, or NULL. An element of an array. */
typedef struct runtime_updates_source_check {
    uint32_t outcome;
    const char *detail;
} runtime_updates_source_check;

/**
 * Each catalog source's check, the versions fetched, each scheduled version that could not be installed or switched,
 * and the versions the sweep removed. First shape ends at `storage`.
 */
typedef struct runtime_updates_check_answer {
    uint32_t size;
    const runtime_updates_source_check *sources;
    size_t source_count;
    const char *const *fetched;
    size_t fetched_count;
    const runtime_updates_problem *refused;
    size_t refused_count;
    const char *const *removed;
    size_t removed_count;
    void *storage;
} runtime_updates_check_answer;

/** The `item`, a host or a plugin, of `host_id` to switch back. First shape ends at `item`. */
typedef struct runtime_updates_rollback_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *host_id;
    const char *item;
} runtime_updates_rollback_argument;

/** What `host_id` records after the rollback as run, next and rollback. First shape ends at `storage`. */
typedef struct runtime_updates_rollback_answer {
    uint32_t size;
    const runtime_updates_version *run;
    size_t run_count;
    const runtime_updates_version *next;
    size_t next_count;
    const runtime_updates_version *rollback;
    size_t rollback_count;
    void *storage;
} runtime_updates_rollback_answer;

/** The host whose recorded run to read. First shape ends at `host_id`. */
typedef struct runtime_updates_recorded_argument {
    uint32_t size;
    const char *state;
    const char *cache;
    const char *host_id;
} runtime_updates_recorded_argument;

/** What the host's last start recorded running, none when it recorded nothing. First shape ends at `storage`. */
typedef struct runtime_updates_recorded_answer {
    uint32_t size;
    const runtime_updates_version *run;
    size_t run_count;
    void *storage;
} runtime_updates_recorded_answer;

/** The places every registered host is checked in, at `now`, an ISO time. First shape ends at `now`. */
typedef struct runtime_updates_run_argument {
    uint32_t size;
    const char *portable;
    const char *state;
    const char *cache;
    const char *now;
} runtime_updates_run_argument;

/** One host's check, as check answers it. An element of an array, so it has no size. */
typedef struct runtime_updates_host_check {
    const char *host_id;
    const runtime_updates_source_check *sources;
    size_t source_count;
    const char *const *fetched;
    size_t fetched_count;
    const runtime_updates_problem *refused;
    size_t refused_count;
    const char *const *removed;
    size_t removed_count;
} runtime_updates_host_check;

/**
 * A loader's application update, as install's application answers it: the runtime_install_application_action carried
 * out, the version, and why that version could not be recorded and how to record it, each NULL for none. An element of
 * an array, so it has no size.
 */
typedef struct runtime_updates_application {
    const char *loader;
    uint32_t operation;
    const char *version;
    const char *unrecorded_detail;
    const char *unrecorded_fix;
} runtime_updates_application;

/**
 * Each registered host's check, each application an install method installed and updated, and each host whose
 * settings, check or update was refused, with its id. First shape ends at `storage`.
 */
typedef struct runtime_updates_run_answer {
    uint32_t size;
    const runtime_updates_host_check *checks;
    size_t check_count;
    const runtime_updates_application *applications;
    size_t application_count;
    const runtime_updates_problem *refused;
    size_t refused_count;
    void *storage;
} runtime_updates_run_answer;

/**
 * The updates service. Every operation answers MALFORMED for an argument short of its first shape, a place that is
 * not absolute, a host id that is not a plain file name, a required text that is NULL or empty, an array whose count
 * has no elements or holds an empty text or an id twice, settings that are not a well-formed record, or a platform
 * that is not a key; such a call has written nothing. The answer arguments are required and the refusal may be NULL
 * for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_updates_definition {
    uint32_t size;
    void *context;
    /**
     * What a start of the host does before it runs anything: switches the host to the version its references schedule
     * once that is installed intact, keeping the one switched from as rollback; resolves the plugins the settings and
     * defaults choose from installed versions only, a plugin whose version changed keeping the one it ran before as
     * its rollback; with libraries, resolves those for the next start and writes the host's boot hint; then records
     * the references and, last, this start's run record. A fetched version switches on the next start, never this one.
     *
     * @return OK; MALFORMED; REFUSED when the host is not registered or its registration cannot be read, the registry
     * lock cannot be taken, or the references or the run record cannot be written, writing nothing before the lock
     * is held; OUT_OF_MEMORY, removing any run record it wrote. A refused or OUT_OF_MEMORY apply may have recorded the
     * references, which the next start would record alike; a boot hint that cannot be written is reported in the
     * answer's unrecorded, never refused.
     */
    spisor_status (*apply)(void *context, const runtime_updates_apply_argument *argument,
                           runtime_updates_apply_answer *answer, spisor_refusal *refusal);
    /**
     * Checks every catalog source the settings name, an https one at most once per updates.intervalHours (24 by
     * default); schedules as next every damaged version named, every newer version of a plugin the settings choose,
     * and the version the host and spisor-host are held to, recording next under the registry lock before anything is
     * fetched; installs each; switches the boot hint of a host it switches to; and sweeps the cache. What runs now is
     * never changed, and a version a rollback moved away from is scheduled again only once the catalog's latest is
     * another one.
     *
     * @return OK, a version that cannot be installed or switched reported in the answer's refused; MALFORMED; REFUSED
     * when the host is not registered, the registry lock cannot be taken, or next cannot be recorded; OUT_OF_MEMORY.
     * A refused or OUT_OF_MEMORY check may have recorded next and installed versions, which the next check keeps.
     */
    spisor_status (*check)(void *context, const runtime_updates_check_argument *argument,
                           runtime_updates_check_answer *answer, spisor_refusal *refusal);
    /**
     * Switches the item back to the version the host's last switch kept as rollback, keeping the one switched away
     * from as rollback in turn and as the version no check schedules again until the catalog moves past it, and makes
     * it the version the item's boot hint names.
     *
     * @return OK; MALFORMED; REFUSED when no rollback is kept for the item, that version is no longer installed intact,
     * the host is not registered, the registry lock cannot be taken, or the references or the boot hint cannot be
     * written, having written nothing; OUT_OF_MEMORY. The references are recorded before the hint and recorded as
     * they were again when the hint cannot be switched, so only memory running out during that undo leaves them
     * rolled back with the hint unswitched.
     */
    spisor_status (*rollback)(void *context, const runtime_updates_rollback_argument *argument,
                              runtime_updates_rollback_answer *answer, spisor_refusal *refusal);
    /**
     * What the host's last start recorded running, so a second process of a running host starts what that host runs
     * without moving it to another version. It writes nothing.
     *
     * @return OK; MALFORMED; REFUSED when the references file exists and cannot be read; OUT_OF_MEMORY.
     */
    spisor_status (*recorded)(void *context, const runtime_updates_recorded_argument *argument,
                              runtime_updates_recorded_answer *answer, spisor_refusal *refusal);
    /**
     * Runs check for every registered host, spisor-host first with the catalog sources its settings record lent to
     * every other check, and install's application update for each loader whose application an install method
     * installed. A host refused is reported with its id and the run goes on.
     *
     * @return OK; MALFORMED; REFUSED when the registered hosts cannot be listed or spisor-host's settings cannot be
     * read; OUT_OF_MEMORY. What a refused or OUT_OF_MEMORY run checked or updated stays as each check or update left
     * it.
     */
    spisor_status (*run)(void *context, const runtime_updates_run_argument *argument,
                         runtime_updates_run_answer *answer, spisor_refusal *refusal);
    void (*release_apply_answer)(void *context, runtime_updates_apply_answer *answer);
    void (*release_check_answer)(void *context, runtime_updates_check_answer *answer);
    void (*release_rollback_answer)(void *context, runtime_updates_rollback_answer *answer);
    void (*release_recorded_answer)(void *context, runtime_updates_recorded_answer *answer);
    void (*release_run_answer)(void *context, runtime_updates_run_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_updates_definition;

/** The static runtime_updates_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_updates_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
