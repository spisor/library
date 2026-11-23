#ifndef RUNTIME_PROCESSES_H
#define RUNTIME_PROCESSES_H

#include "cJSON.h"
#include "process_start.h"

/**
 * @file
 * The processes service's functions, for its definition: each takes an absolute state and, where it takes one, a
 * plain host id, and answers its result, or NULL with *refusal set to {message, fix}, or with it NULL when memory
 * runs out.
 */

/** What processes' hold holds: holder, a pid above zero, holds background host host_id under state. */
typedef struct runtime_processes_hold_request {
    const char *state;
    const char *host_id;
    double holder;
    runtime_detached_start start;
} runtime_processes_hold_request;

/**
 * processes' hold: makes the request's holder a holder of its host, dropping holders that died, and starts the host
 * detached through start when no process recorded for it runs by its stamp; answers {pid, started}, with outsideJob
 * beside a started one (false only when a Windows job the caller runs in forbade breaking away from it). A start
 * with no directory runs in state, so the host never holds a folder of the user's; one that is not absolute is
 * refused.
 */
cJSON *runtime_processes_hold(const runtime_processes_hold_request *request, cJSON **refusal);

/**
 * processes' release: drops holder, and every holder that died, from background host host_id; answers {holders},
 * the ones left. The host is never signalled: it sees no live holder at its next prune and stops itself.
 */
cJSON *runtime_processes_release(const char *state, const char *host_id, double holder, cJSON **refusal);

/**
 * One prune of background host host_id, whose process is self: at `*now`, or at the current time when `now` is NULL,
 * so a caller passing its own time never has the clock read; a check_milliseconds of 0 takes its default.
 */
typedef struct runtime_processes_prune_request {
    const char *state;
    const char *host_id;
    double self;
    const double *now;
    double check_milliseconds;
} runtime_processes_prune_request;

/**
 * processes' prune, one check of the background host its language layer makes every check_milliseconds (5000 by
 * default): answers {exit: true} when another live process is recorded as the host, or no holder is alive;
 * otherwise drops the dead holders, stamps self alive at the request's time and answers {exit: false}. A recorded
 * host whose process is dead counts as no host, so a host that crashed or ended never ends the next one.
 *
 * @implNote With no holder left the record is removed before the lock is released, so a holder
 * arriving next starts a new host rather than holding one that is about to exit.
 */
cJSON *runtime_processes_prune(const runtime_processes_prune_request *request, cJSON **refusal);

/**
 * A run of host_id to record: run, an object of item ids to versions; check_milliseconds, 0 for 5000; record, a path
 * an earlier recording answered, or NULL for a new record; now, the time a stamp of record is alive at, NULL for the
 * current time, which a new record always takes.
 */
typedef struct runtime_processes_record_request {
    const char *state;
    const char *host_id;
    const cJSON *run;
    double check_milliseconds;
    const char *record;
    const double *now;
} runtime_processes_record_request;

/**
 * processes' recordRun: records that this process runs run as a start of host_id, stamped alive now; answers
 * {record}, its path. With record it stamps that record again instead, which the start's timer does every
 * check_milliseconds until it removes it.
 */
cJSON *runtime_processes_record_run(const runtime_processes_record_request *request, cJSON **refusal);

/**
 * processes' liveRuns: answers {runs}, what every live start of any host runs at now, 0 for the current time,
 * deleting each run record whose process is gone or stopped stamping, as the cache sweep does.
 */
cJSON *runtime_processes_live_runs(const char *state, double now, cJSON **refusal);

/** processes' removeRuns: deletes the run record record, or every run record of host_id without it; 1 when done. */
int runtime_processes_remove_runs(const char *state, const char *host_id, const char *record, cJSON **refusal);

#endif
