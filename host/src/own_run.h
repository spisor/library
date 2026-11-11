#ifndef HOST_OWN_RUN_H
#define HOST_OWN_RUN_H

#include "host_context.h"

/** How often a long-running command stamps the record of the spisor-host version it runs. */
#define HOST_OWN_RUN_INTERVAL_MILLISECONDS 5000u

/** The run record of the spisor-host version this process runs, or none when it runs from outside the cache. */
typedef struct own_run {
    const host_context *context;
    char *version;
    char *record;
} own_run;

/**
 * Records that this process runs the spisor-host version whose bundle holds its executable, so no sweep
 * takes that version while it runs; nothing when the executable lies outside cache/bundles/spisor-host.
 *
 * @implNote A launch can outlive two updates: the first switches the boot hint away from this version and
 * the second drops it from the references, so only this record keeps it. host_stamp_own_run must stamp it
 * every HOST_OWN_RUN_INTERVAL_MILLISECONDS, or the sweep counts it dead after three intervals.
 */
void host_start_own_run(own_run *run, const host_context *context);

void host_stamp_own_run(own_run *run);

/** Removes the record, once this process no longer runs anything from the cache. */
void host_finish_own_run(own_run *run);

#endif
