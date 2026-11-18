#ifndef RUNTIME_PROCESSES_HOLDERS_H
#define RUNTIME_PROCESSES_HOLDERS_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "holders.h"

/** Whether environment is an object of strings whose names are not empty, hold no = and appear once. */
int runtime_processes_is_environment(const cJSON *environment);

/** The holders as a JSON array; NULL when memory runs out. */
cJSON *runtime_processes_holders_array(const double *holders, size_t count);

/**
 * Writes the holders record at path and answers `answer`, built before the write so memory running out never follows
 * it; NULL with *refusal set when the file cannot be written, and NULL out of memory, `answer` deleted either way.
 */
cJSON *runtime_processes_write_answered(const char *path, const runtime_holders_record *record, cJSON *answer,
                                        cJSON **refusal);

/** The refusal of a holders file or lock that could not be used under state. */
cJSON *runtime_processes_state_refusal(const char *what, const char *path);

/** One read-modify-write of a background host's holders, the file at path read into *record, for its context. */
typedef cJSON *(*runtime_holders_step)(const void *context, const char *path, runtime_holders_record *record,
                                       cJSON **refusal);

/**
 * Runs step for context holding the holders lock of host_id under state, with what its holders file records, making
 * the processes folder first when it is missing.
 */
cJSON *runtime_processes_with_holders(const char *state, const char *host_id, runtime_holders_step step,
                                      const void *context, cJSON **refusal);

#endif
