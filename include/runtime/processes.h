#ifndef RUNTIME_PUBLIC_PROCESSES_H
#define RUNTIME_PUBLIC_PROCESSES_H

/**
 * @file
 * runtime's `processes` service: the background hosts kept alive while a holder process lives, recorded under
 * state/processes/, the readiness a session host publishes there, and the run records under state/bundles/runs/ that
 * name what every live start runs. A size-prefixed struct is read only up to its `size`. Text the caller hands in is
 * NUL-terminated UTF-8 borrowed for the call; what an answer or a refusal points at is the provider's until the
 * release for its kind. A state is an absolute directory, a host id a plain file name, and a process id above zero.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/item_version.h"
#include "runtime/environment.h"
#include "spisor/status.h"
#include "spisor/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Process `holder` holding the background host `host_id`, started when no process recorded for it runs as the
 * absolute `executable` with its arguments, in `directory`, NULL for the state. A NULL `environment` with a count of 0
 * inherits the caller's environment; any other replaces it, so variables given with a count of 0 start the host with
 * an empty one. First shape ends at `directory`.
 */
typedef struct runtime_processes_hold_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    int64_t holder;
    const char *executable;
    const char *const *arguments;
    size_t argument_count;
    const runtime_variable *environment;
    size_t environment_count;
    const char *directory;
} runtime_processes_hold_argument;

/**
 * The host's process, whether this call started it, 0 or 1, and for one it started whether it runs outside the
 * caller's Windows job, 0 only when that job forbade breaking away. First shape ends at `storage`.
 */
typedef struct runtime_processes_hold_answer {
    uint32_t size;
    int64_t pid;
    uint32_t started;
    uint32_t outside_job;
    void *storage;
} runtime_processes_hold_answer;

/** Process `holder` of the background host `host_id`. First shape ends at `holder`. */
typedef struct runtime_processes_holder_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    int64_t holder;
} runtime_processes_holder_argument;

/** The holders left. First shape ends at `storage`. */
typedef struct runtime_processes_release_answer {
    uint32_t size;
    const int64_t *holders;
    size_t holder_count;
    void *storage;
} runtime_processes_release_answer;

/**
 * One check of the background host running as process `self`, at `now` in milliseconds since the epoch, 0 for the
 * current time, by a host that checks every `check_milliseconds`, 0 for 5000. The stamp it writes, the stamp it
 * judges and the age of the holders lock are all read at `now`. First shape ends at `check_milliseconds`.
 */
typedef struct runtime_processes_prune_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    int64_t self;
    double now;
    double check_milliseconds;
} runtime_processes_prune_argument;

/** Whether the host is to exit, 0 or 1. First shape ends at `storage`. */
typedef struct runtime_processes_prune_answer {
    uint32_t size;
    uint32_t exit;
    void *storage;
} runtime_processes_prune_answer;

/**
 * The session host `host_id`, running as `pid`, ready with the environment the application is started with, NULL for
 * none, or, with `refused` and its `fix`, unable to serve, which excludes an environment. First shape ends at `fix`.
 */
typedef struct runtime_processes_ready_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    int64_t pid;
    const runtime_variable *environment;
    size_t environment_count;
    const char *refused;
    const char *fix;
} runtime_processes_ready_argument;

/**
 * The readiness of `host_id` running as `pid` to wait for, at most `timeout_milliseconds`. First shape ends at
 * `timeout_milliseconds`.
 */
typedef struct runtime_processes_await_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    int64_t pid;
    int64_t timeout_milliseconds;
} runtime_processes_await_argument;

/** The environment the ready host published. First shape ends at `storage`. */
typedef struct runtime_processes_await_answer {
    uint32_t size;
    const runtime_variable *environment;
    size_t environment_count;
    void *storage;
} runtime_processes_await_answer;

/**
 * What this process runs as a start of `host_id`, stamped by a timer every `check_milliseconds`, 0 for 5000; with
 * `record`, a path an earlier call answered, that record is stamped again instead, NULL to write a new one. First shape
 * ends at `record`.
 */
typedef struct runtime_processes_record_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    const runtime_item_version *run;
    size_t run_count;
    double check_milliseconds;
    const char *record;
} runtime_processes_record_argument;

/** The run record's path. First shape ends at `storage`. */
typedef struct runtime_processes_record_answer {
    uint32_t size;
    const char *record;
    void *storage;
} runtime_processes_record_answer;

/** The state whose live runs to read at `now`, 0 for the current time. First shape ends at `now`. */
typedef struct runtime_processes_live_argument {
    uint32_t size;
    const char *state;
    double now;
} runtime_processes_live_argument;

/** What one live start runs. An element of an array, so it has no size. */
typedef struct runtime_processes_run {
    const runtime_item_version *versions;
    size_t version_count;
} runtime_processes_run;

/** What every live start of any host runs, in host and file name order. First shape ends at `storage`. */
typedef struct runtime_processes_live_answer {
    uint32_t size;
    const runtime_processes_run *runs;
    size_t run_count;
    void *storage;
} runtime_processes_live_answer;

/** The run record `record` of `host_id` to delete, NULL for every one of them. First shape ends at `record`. */
typedef struct runtime_processes_remove_argument {
    uint32_t size;
    const char *state;
    const char *host_id;
    const char *record;
} runtime_processes_remove_argument;

/**
 * The processes service. Every operation answers MALFORMED for an argument short of its first shape, a state that is
 * not absolute, a host id that is not a plain file name, a required text that is NULL or empty, a process id that is
 * not above zero, a time that is negative or not finite, an array whose count has no elements or holds a NULL or empty
 * text or an id twice, or an environment as runtime/environment.h refuses one; such a call has written nothing. The one
 * array whose texts may be empty is hold's `arguments`, since a program may take an empty argument; it still refuses
 * NULL. Memory running out never reads as a process that is gone or a record that is not live, so it never deletes a
 * record of a live process and never starts a second host. The answer arguments are required and the refusal may be
 * NULL for none. First shape ends at `release_refusal`.
 */
typedef struct runtime_processes_definition {
    uint32_t size;
    void *context;
    /**
     * Makes the holder a holder of the background host, dropping holders that died, and starts the host detached when
     * no process recorded for it runs by its stamp.
     *
     * @return OK; MALFORMED; REFUSED when the holders or the lock cannot be used under the state, or the host cannot
     * be started; OUT_OF_MEMORY, having started nothing and recorded no holder. A host it started whose record then
     * cannot be written is stopped before it answers REFUSED, so no answer but OK leaves a host running unrecorded.
     */
    spisor_status (*hold)(void *context, const runtime_processes_hold_argument *argument,
                          runtime_processes_hold_answer *answer, spisor_refusal *refusal);
    /**
     * Drops the holder and every holder that died. The host is never signalled: it sees no live holder at its next
     * prune and stops itself.
     *
     * @return OK; MALFORMED; REFUSED when the holders or the lock cannot be used; OUT_OF_MEMORY, possibly having
     * dropped the holder and the dead ones, as a repeated release does.
     */
    spisor_status (*release)(void *context, const runtime_processes_holder_argument *argument,
                             runtime_processes_release_answer *answer, spisor_refusal *refusal);
    /**
     * One check of the background host: exit when another process is recorded as the host and its stamp is fresh, or
     * no holder is alive, otherwise drop the dead holders and stamp self alive. A recorded host whose stamp is stale
     * counts as no host, even when an unrelated process now has its pid. With no holder left the record is removed
     * before the lock is released, so a holder arriving next starts a new host rather than holding one about to exit.
     *
     * @return OK; MALFORMED; REFUSED when the holders or the lock cannot be used; OUT_OF_MEMORY, having changed nothing
     * and never answering exit for a host whose holders live.
     */
    spisor_status (*prune)(void *context, const runtime_processes_prune_argument *argument,
                           runtime_processes_prune_answer *answer, spisor_refusal *refusal);
    /**
     * Publishes that the session host is ready, or cannot serve, by writing state/processes/<host id>.ready.json
     * whole, naming its pid so a reader ignores a record an earlier run left.
     *
     * @return OK; MALFORMED, also for a refusal without its fix or beside an environment; REFUSED when the record
     * cannot be written; OUT_OF_MEMORY, having written nothing.
     */
    spisor_status (*ready)(void *context, const runtime_processes_ready_argument *argument, spisor_refusal *refusal);
    /**
     * Polls every 50 milliseconds for the readiness of the host naming the pid, ignoring one that names another. It
     * writes nothing.
     *
     * @return OK; MALFORMED; REFUSED as a published refusal was published, or naming the host when no record appears
     * in time; OUT_OF_MEMORY.
     */
    spisor_status (*await_ready)(void *context, const runtime_processes_await_argument *argument,
                                 runtime_processes_await_answer *answer, spisor_refusal *refusal);
    /**
     * Records what this process runs as a start of the host, stamped alive now, or stamps the record given again.
     *
     * @return OK; MALFORMED; REFUSED when the record given is no run record of the host, or the record cannot be
     * written; OUT_OF_MEMORY, having written no new record.
     */
    spisor_status (*record_run)(void *context, const runtime_processes_record_argument *argument,
                                runtime_processes_record_answer *answer, spisor_refusal *refusal);
    /**
     * What every live start runs, deleting each run record whose process is gone, which stopped being stamped, or
     * which is not the shape of one, as the cache sweep does.
     *
     * @return OK; MALFORMED; OUT_OF_MEMORY, having deleted only records that were not live.
     */
    spisor_status (*live_runs)(void *context, const runtime_processes_live_argument *argument,
                               runtime_processes_live_answer *answer, spisor_refusal *refusal);
    /**
     * Deletes the run record given, or every run record of the host.
     *
     * @return OK; MALFORMED; REFUSED when the record is no run record of the host or a record cannot be deleted;
     * OUT_OF_MEMORY, having deleted nothing.
     */
    spisor_status (*remove_runs)(void *context, const runtime_processes_remove_argument *argument,
                                 spisor_refusal *refusal);
    void (*release_hold_answer)(void *context, runtime_processes_hold_answer *answer);
    void (*release_release_answer)(void *context, runtime_processes_release_answer *answer);
    void (*release_prune_answer)(void *context, runtime_processes_prune_answer *answer);
    void (*release_await_answer)(void *context, runtime_processes_await_answer *answer);
    void (*release_record_answer)(void *context, runtime_processes_record_answer *answer);
    void (*release_live_answer)(void *context, runtime_processes_live_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_processes_definition;

/** The static runtime_processes_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_processes_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
