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

/**
 * Whether record is the path of one run record of host_id, a .json file named directly inside its
 * folder under state/bundles/runs/, as runtime_record_run answers it.
 */
int runtime_is_run_record_of(const char *state, const char *host_id, const char *record);

typedef enum runtime_run_stamp {
    RUNTIME_RUN_STAMPED,
    RUNTIME_RUN_NOT_A_RECORD,
    RUNTIME_RUN_UNWRITABLE
} runtime_run_stamp;

/**
 * Stamps the run record at record alive again, now, as runtime_record_run wrote it, making its folder
 * again when a removal took it. NOT_A_RECORD, writing nothing, when record is no run record of host_id.
 */
runtime_run_stamp runtime_stamp_run(const char *state, const char *host_id, const char *record, const cJSON *run,
                                    double check_milliseconds);

/**
 * What every live start of any host runs, as an array of id-to-version objects in host and file name
 * order, deleting each run record whose process is gone, which stopped being stamped for three of its
 * intervals, or which is not the shape of one. NULL when memory runs out.
 *
 * @implNote This is the format's one reader: the cache sweep keeps what it answers.
 */
cJSON *runtime_live_runs(const char *state, double now);

/** Deletes every run record of host_id; a host with none counts as removed. */
int runtime_remove_run_records(const char *state, const char *host_id);

/** Deletes the run record at record; 0 when it is no run record of host_id or could not be deleted. */
int runtime_remove_run_record(const char *state, const char *host_id, const char *record);

#endif
