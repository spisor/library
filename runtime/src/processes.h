#ifndef RUNTIME_PROCESSES_H
#define RUNTIME_PROCESSES_H

#include "cJSON.h"

/**
 * @file
 * The processes service's operations over JSON, for runtime's own callers: each answers its result, or NULL with
 * *refusal set to {message, fix}, or with it NULL when memory runs out.
 */

/**
 * processes' hold: {state, hostId, holder, start: {executable, arguments?, environment?, directory?}}
 * makes the process holder a holder of background host hostId, dropping holders that died, and starts
 * the host detached through start when no process recorded for it runs by its stamp; answers
 * {pid, started}, with outsideJob beside a started one (false only when a Windows job the caller runs in
 * forbade breaking away from it). start is built from the start the host's registration records; directory
 * defaults to state, so the host never holds a folder of the user's.
 */
cJSON *runtime_processes_hold_operation(const cJSON *argument, cJSON **refusal);

/**
 * processes' release: {state, hostId, holder} drops holder, and every holder that died, from background
 * host hostId; answers {holders}, the ones left. The host is never signalled: it sees no live holder at
 * its next prune and stops itself.
 */
cJSON *runtime_processes_release_operation(const cJSON *argument, cJSON **refusal);

/**
 * processes' prune: {state, hostId, self, now?, checkMilliseconds?}, one check of the background host
 * whose process is self, which its language layer makes every checkMilliseconds (5000 by default):
 * answers {exit: true} when another process is recorded as the host, or no holder is alive; otherwise
 * drops the dead holders, stamps self alive at now (the current time by default) and answers
 * {exit: false}.
 *
 * @implNote With no holder left the record is removed before the lock is released, so a holder
 * arriving next starts a new host rather than holding one that is about to exit.
 */
cJSON *runtime_processes_prune_operation(const cJSON *argument, cJSON **refusal);

/**
 * processes' recordRun: {state, hostId, run, checkMilliseconds?, record?} records that this process runs
 * run, an id-to-version object, as a start of hostId, stamped alive now; answers {record}, its path.
 * With record, a path an earlier call answered, it stamps that record again instead, which the start's
 * timer does every checkMilliseconds (5000 by default) until it removes it.
 */
cJSON *runtime_processes_record_run_operation(const cJSON *argument, cJSON **refusal);

/**
 * processes' liveRuns: {state, now?} answers {runs}, what every live start of any host runs, deleting
 * each run record whose process is gone or stopped stamping, as the cache sweep does.
 */
cJSON *runtime_processes_live_runs_operation(const cJSON *argument, cJSON **refusal);

/**
 * processes' removeRuns: {state, hostId, record?} deletes the run record record, or every run record of
 * hostId without it; answers {}.
 */
cJSON *runtime_processes_remove_runs_operation(const cJSON *argument, cJSON **refusal);

#endif
