#include <math.h>
#include <stdlib.h>

#include "cache_refusal.h"
#include "allocation.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "json_service.h"
#include "node_json_file.h"
#include "processes.h"
#include "processes_holders.h"
#include "run_records.h"

#define RECORD_FIX "send record as a path processes.recordRun answered for this host, or leave it out"
#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"

static cJSON *record_answer(const char *record) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddStringToObject(answer, "record", record) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *not_a_record(const char *record) {
    const char *parts[2] = {"no run record of this host is at ", record};
    return runtime_cache_refusal(parts, 2, RECORD_FIX);
}

static cJSON *unwritable(const char *host_id) {
    const char *parts[3] = {"the run record of ", host_id, " could not be written"};
    return runtime_cache_refusal(parts, 3, WRITABLE_STATE_FIX);
}

static cJSON *stamp_again(const runtime_processes_record_request *request, double check, cJSON **refusal) {
    runtime_run_stamp stamped =
        runtime_stamp_run(request->state, request->host_id, request->record, request->run, check);
    if (stamped != RUNTIME_RUN_STAMPED) {
        *refusal = stamped == RUNTIME_RUN_NOT_A_RECORD ? not_a_record(request->record)
                   : stamped == RUNTIME_RUN_UNWRITABLE ? unwritable(request->host_id)
                                                       : NULL;
        return NULL;
    }
    return record_answer(request->record);
}

cJSON *runtime_processes_record_run(const runtime_processes_record_request *request, cJSON **refusal) {
    double check =
        request->check_milliseconds == 0 ? RUNTIME_DEFAULT_CHECK_MILLISECONDS : request->check_milliseconds;
    *refusal = NULL;
    if (request->record != NULL) {
        return stamp_again(request, check, refusal);
    }
    int out_of_memory = 0;
    spisor_prepared_path removal;
    char *path = runtime_record_run(request->state, request->host_id, request->run, check, &removal, &out_of_memory);
    if (path == NULL) {
        *refusal = out_of_memory ? NULL : unwritable(request->host_id);
        return NULL;
    }
    cJSON *answer = record_answer(path);
    if (answer == NULL) {
        spisor_remove_prepared_file(&removal);
    }
    spisor_release_prepared_path(&removal);
    spisor_memory_release(path);
    return answer;
}

cJSON *runtime_processes_live_runs(const char *state, double now, cJSON **refusal) {
    cJSON *answer = cJSON_CreateObject();
    double at = now == 0 ? runtime_now_milliseconds() : now;
    *refusal = NULL;
    if (answer == NULL || !runtime_add_owned(answer, "runs", runtime_live_runs(state, at))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

int runtime_processes_remove_runs(const char *state, const char *host_id, const char *record, cJSON **refusal) {
    int out_of_memory = 0;
    *refusal = NULL;
    if (record != NULL && !runtime_is_run_record_of(state, host_id, record, &out_of_memory)) {
        *refusal = out_of_memory ? NULL : not_a_record(record);
        return 0;
    }
    spisor_status removed = record == NULL ? runtime_remove_run_records(state, host_id)
                                           : runtime_remove_run_record(state, host_id, record);
    if (removed != SPISOR_OK && removed != SPISOR_OUT_OF_MEMORY) {
        const char *parts[3] = {"the run records of ", host_id, " could not be removed"};
        *refusal = runtime_cache_refusal(parts, 3, WRITABLE_STATE_FIX);
    }
    return removed == SPISOR_OK;
}
