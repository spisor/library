#include <math.h>
#include <stdlib.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "node_json_file.h"
#include "processes_request.h"
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

static cJSON *stamp_again(const runtime_processes_request *request, const char *record, const cJSON *run,
                          double check, cJSON **refusal) {
    if (!runtime_is_run_record_of(request->state, request->host_id, record)) {
        *refusal = not_a_record(record);
        return NULL;
    }
    if (!runtime_stamp_run(request->state, request->host_id, record, run, check)) {
        *refusal = unwritable(request->host_id);
        return NULL;
    }
    return record_answer(record);
}

cJSON *runtime_processes_record_run_for(const runtime_processes_request *request, cJSON **refusal) {
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(request->argument, "run");
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(request->argument, "record");
    double check = runtime_processes_positive_member(request->argument, "checkMilliseconds",
                                                     RUNTIME_DEFAULT_CHECK_MILLISECONDS);
    if (!cJSON_IsObject(run) || !runtime_is_version_map(run)) {
        *refusal = runtime_processes_argument_refusal("run", "an object of item ids to the versions this start runs");
        return NULL;
    }
    if (isnan(check)) {
        *refusal = runtime_processes_argument_refusal("checkMilliseconds", "milliseconds above zero, or leave it out");
        return NULL;
    }
    if (record != NULL && !cJSON_IsString(record)) {
        *refusal = runtime_processes_argument_refusal("record", "the path processes.recordRun answered, or leave it out");
        return NULL;
    }
    if (record != NULL) {
        return stamp_again(request, record->valuestring, run, check, refusal);
    }
    char *path = runtime_record_run(request->state, request->host_id, run, check);
    if (path == NULL) {
        *refusal = unwritable(request->host_id);
        return NULL;
    }
    cJSON *answer = record_answer(path);
    free(path);
    return answer;
}

cJSON *runtime_processes_live_runs_for(const runtime_processes_request *request, cJSON **refusal) {
    double now = runtime_processes_positive_member(request->argument, "now", runtime_now_milliseconds());
    if (isnan(now)) {
        *refusal = runtime_processes_argument_refusal("now", "milliseconds since the epoch, or leave it out");
        return NULL;
    }
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || !runtime_catalog_attach(answer, "runs", runtime_live_runs(request->state, now))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_processes_remove_runs_for(const runtime_processes_request *request, cJSON **refusal) {
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(request->argument, "record");
    if (record != NULL && (!cJSON_IsString(record) ||
                           !runtime_is_run_record_of(request->state, request->host_id, record->valuestring))) {
        *refusal = not_a_record(cJSON_IsString(record) ? record->valuestring : "a value that is no path");
        return NULL;
    }
    int removed = record == NULL ? runtime_remove_run_records(request->state, request->host_id)
                                 : runtime_remove_run_record(request->state, request->host_id, record->valuestring);
    if (!removed) {
        const char *parts[3] = {"the run records of ", request->host_id, " could not be removed"};
        *refusal = runtime_cache_refusal(parts, 3, WRITABLE_STATE_FIX);
        return NULL;
    }
    return cJSON_CreateObject();
}
