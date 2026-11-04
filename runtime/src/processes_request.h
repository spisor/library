#ifndef RUNTIME_PROCESSES_REQUEST_H
#define RUNTIME_PROCESSES_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "holders.h"

/** Which host id a processes function takes: none, a background host's, or a start's runs. */
typedef enum runtime_processes_host {
    RUNTIME_PROCESSES_NO_HOST,
    RUNTIME_PROCESSES_BACKGROUND_HOST,
    RUNTIME_PROCESSES_RUNS_HOST
} runtime_processes_host;

/** One processes call: its argument, an absolute state and, unless the function takes none, a plain host id. */
typedef struct runtime_processes_request {
    const cJSON *argument;
    const char *state;
    const char *host_id;
} runtime_processes_request;

/** Answers the result, or NULL with *refusal set to {message, fix}; NULL with no refusal is out of memory. */
typedef cJSON *(*runtime_processes_operation)(const runtime_processes_request *request, cJSON **refusal);

/** Parses one json call, checks state and hostId, and serves it through operation. */
int32_t runtime_processes_serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                runtime_processes_host host, runtime_processes_operation operation);

/** The refusal of an argument that carries no usable member, saying what shape it takes. */
cJSON *runtime_processes_argument_refusal(const char *member, const char *shape);

/** The pid member, a whole number above zero, or NaN. */
double runtime_processes_pid_member(const cJSON *argument, const char *name);

/** The number member, or fallback when it is absent; NaN when it is there and no finite number above zero. */
double runtime_processes_positive_member(const cJSON *argument, const char *name, double fallback);

/** The holders as a JSON array; NULL when memory runs out. */
cJSON *runtime_processes_holders_array(const double *holders, size_t count);

/** The refusal of a holders file or lock that could not be used under state. */
cJSON *runtime_processes_state_refusal(const char *what, const char *path);

/** One read-modify-write of a background host's holders, the file at path read into *record. */
typedef cJSON *(*runtime_holders_step)(const runtime_processes_request *request, const char *path,
                                       runtime_holders_record *record, cJSON **refusal);

/**
 * Runs step holding the host's holders lock, with what its holders file records, making the processes
 * folder first when it is missing.
 */
cJSON *runtime_processes_with_holders(const runtime_processes_request *request, runtime_holders_step step,
                                      cJSON **refusal);

cJSON *runtime_processes_hold_for(const runtime_processes_request *request, cJSON **refusal);

cJSON *runtime_processes_record_run_for(const runtime_processes_request *request, cJSON **refusal);

cJSON *runtime_processes_live_runs_for(const runtime_processes_request *request, cJSON **refusal);

cJSON *runtime_processes_remove_runs_for(const runtime_processes_request *request, cJSON **refusal);

#endif
