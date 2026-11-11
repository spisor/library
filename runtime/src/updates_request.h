#ifndef RUNTIME_UPDATES_REQUEST_H
#define RUNTIME_UPDATES_REQUEST_H

#include "cJSON.h"
#include "hosts_lock.h"

/** One updates call: its argument, the places it acts in, the host it acts for and the platform key. */
typedef struct runtime_updates_request {
    const cJSON *argument;
    const char *state;
    const char *cache;
    const char *host_id;
    const char *platform;
} runtime_updates_request;

/** An updates operation: its answer, or NULL with *refusal set to {message, fix}, or NULL when memory ran out. */
typedef cJSON *(*runtime_updates_operation)(const runtime_updates_request *request, cJSON **refusal);

cJSON *runtime_updates_apply_for(const runtime_updates_request *request, cJSON **refusal);
cJSON *runtime_updates_check_for(const runtime_updates_request *request, cJSON **refusal);
cJSON *runtime_updates_rollback_for(const runtime_updates_request *request, cJSON **refusal);
cJSON *runtime_updates_recorded_for(const runtime_updates_request *request, cJSON **refusal);

/** The registry lock under request's state, or NULL with *refusal saying it could not be taken. */
spisor_lock *runtime_updates_lock(const runtime_updates_request *request, cJSON **refusal);

/**
 * The registry lock, taken only while request's host is registered: NULL with *refusal set when it is
 * not, or when its registration cannot be read, and with *refusal NULL when memory runs out.
 *
 * @implNote Every update write takes this, so a host still running after hosts.remove deleted its state
 * under the same lock cannot record references, run records or hints the sweep would keep for ever.
 */
spisor_lock *runtime_updates_lock_registered(const runtime_updates_request *request, cJSON **refusal);

/** The refusal of a references file that could not be written, taking problem, which it frees. */
cJSON *runtime_updates_unrecorded(char *problem);

#endif
