#include "install_references.h"

#include <stdlib.h>

#include "host_references.h"
#include "hosts_lock.h"
#include "json_service.h"

#define STATE_FIX "check that SPISOR_STATE names a writable folder, then run install again"

typedef int (*run_change)(runtime_host_references *references, const cJSON *versions);

static int merge_versions(runtime_host_references *references, const cJSON *versions) {
    for (const cJSON *version = versions->child; version != NULL; version = version->next) {
        cJSON *copy = cJSON_CreateString(version->valuestring);
        cJSON_DeleteItemFromObjectCaseSensitive(references->run, version->string);
        if (copy == NULL || !cJSON_AddItemToObject(references->run, version->string, copy)) {
            cJSON_Delete(copy);
            return 0;
        }
    }
    return 1;
}

static int replace_run(runtime_host_references *references, const cJSON *run) {
    cJSON *copy = cJSON_Duplicate(run, 1);
    if (copy == NULL) {
        return 0;
    }
    cJSON_Delete(references->run);
    references->run = copy;
    return 1;
}

static int change_under_lock(const char *state, const char *host_id, const cJSON *versions, run_change change,
                             cJSON **refusal) {
    spisor_lock *guard = runtime_hosts_lock(state);
    if (guard == NULL) {
        *refusal = runtime_refusal("the registry lock under the state folder could not be taken", STATE_FIX);
        return 0;
    }
    runtime_host_references references = runtime_read_host_references(state, host_id);
    char *problem = NULL;
    int recorded = references.run != NULL && references.next != NULL && change(&references, versions) &&
                   runtime_record_host_references(state, host_id, &references, &problem);
    runtime_free_host_references(&references);
    spisor_unlock(guard);
    if (!recorded && problem != NULL) {
        *refusal = runtime_refusal(problem, STATE_FIX);
    }
    free(problem);
    return recorded;
}

int runtime_install_reference_versions(const char *state, const char *host_id, const cJSON *versions, cJSON **refusal) {
    return change_under_lock(state, host_id, versions, merge_versions, refusal);
}

int runtime_install_replace_run(const char *state, const char *host_id, const cJSON *run, cJSON **refusal) {
    return change_under_lock(state, host_id, run, replace_run, refusal);
}
