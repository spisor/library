#ifndef RUNTIME_RUN_RECORDS_H
#define RUNTIME_RUN_RECORDS_H

#include "cJSON.h"

/** How often a start stamps its run record alive unless told otherwise, as a background host's holders are. */
#define RUNTIME_DEFAULT_CHECK_MILLISECONDS 5000.0

/**
 * Records that this process runs run as a start of host_id, as
 * JSON.stringify({pid, aliveAt, checkMilliseconds, run}) and a newline in its own file under
 * state/bundles/runs/<host_id>/, and answers that file's path, freed with free. NULL when host_id is no
 * plain file name, the file cannot be written or memory runs out.
 *
 * @implNote One file per start, named by the process id and a number no other start of this process
 * takes, so two starts of one host on different versions never overwrite each other's: a sweep keeps
 * every version any live start names. The language layer stamps it again on its own timer and removes
 * it when the start stops.
 */
char *runtime_record_run(const char *state, const char *host_id, const cJSON *run, double check_milliseconds);

/** Deletes every run record of host_id; a host with none counts as removed. */
int runtime_remove_run_records(const char *state, const char *host_id);

#endif
