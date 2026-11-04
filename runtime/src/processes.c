#include "processes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "holders_lock.h"
#include "host_references.h"
#include "paths.h"
#include "processes_request.h"
#include "run_records.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"

cJSON *runtime_processes_argument_refusal(const char *member, const char *shape) {
    const char *message[2] = {"the argument carries no usable ", member};
    const char *fix[4] = {"send ", member, " as ", shape};
    char *fix_text = runtime_cache_text(fix, 4);
    cJSON *refusal = fix_text == NULL ? NULL : runtime_cache_refusal(message, 2, fix_text);
    free(fix_text);
    return refusal;
}

cJSON *runtime_processes_state_refusal(const char *what, const char *path) {
    const char *parts[4] = {what, " at ", path, " could not be used"};
    return runtime_cache_refusal(parts, 4, WRITABLE_STATE_FIX);
}

double runtime_processes_pid_member(const cJSON *argument, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(argument, name);
    int is_pid = cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble > 0 &&
                 floor(value->valuedouble) == value->valuedouble;
    return is_pid ? value->valuedouble : NAN;
}

double runtime_processes_positive_member(const cJSON *argument, const char *name, double fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(argument, name);
    if (value == NULL) {
        return fallback;
    }
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble > 0 ? value->valuedouble : NAN;
}

cJSON *runtime_processes_holders_array(const double *holders, size_t count) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; array != NULL && index < count; index++) {
        cJSON *holder = cJSON_CreateNumber(holders[index]);
        if (holder == NULL || !cJSON_AddItemToArray(array, holder)) {
            cJSON_Delete(holder);
            cJSON_Delete(array);
            array = NULL;
        }
    }
    return array;
}

static cJSON *locked_step(const runtime_processes_request *request, const char *path, runtime_holders_step step,
                          cJSON **refusal) {
    char *problem = NULL;
    char *lock = runtime_holders_path(request->state, request->host_id, ".lock", &problem);
    runtime_holders_guard *guard = lock == NULL ? NULL : runtime_lock_holders(lock);
    if (guard == NULL) {
        *refusal = lock == NULL ? NULL : runtime_processes_state_refusal("the holders lock", lock);
        free(lock);
        free(problem);
        return NULL;
    }
    runtime_holders_record record = runtime_no_holders();
    cJSON *answer = NULL;
    if (runtime_read_holders(path, &record)) {
        answer = step(request, path, &record, refusal);
    }
    runtime_free_holders(&record);
    runtime_unlock_holders(guard);
    free(lock);
    return answer;
}

cJSON *runtime_processes_with_holders(const runtime_processes_request *request, runtime_holders_step step,
                                      cJSON **refusal) {
    char *problem = NULL;
    char *path = runtime_holders_path(request->state, request->host_id, RUNTIME_JSON_EXTENSION, &problem);
    char *folder = runtime_processes_folder(request->state);
    cJSON *answer = NULL;
    if (path != NULL && folder != NULL && !spisor_make_directories(folder)) {
        *refusal = runtime_processes_state_refusal("the processes folder", folder);
    } else if (path != NULL && folder != NULL) {
        answer = locked_step(request, path, step, refusal);
    }
    free(folder);
    free(path);
    free(problem);
    return answer;
}

static cJSON *write_refusal(const char *path) {
    return runtime_processes_state_refusal("the holders file", path);
}

static cJSON *release_step(const runtime_processes_request *request, const char *path, runtime_holders_record *record,
                           cJSON **refusal) {
    runtime_keep_live_holders(record, runtime_processes_pid_member(request->argument, "holder"));
    if (!runtime_write_holders(path, record)) {
        *refusal = write_refusal(path);
        return NULL;
    }
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL &&
        !runtime_catalog_attach(answer, "holders", runtime_processes_holders_array(record->holders, record->count))) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *release_for(const runtime_processes_request *request, cJSON **refusal) {
    if (isnan(runtime_processes_pid_member(request->argument, "holder"))) {
        *refusal = runtime_processes_argument_refusal("holder", "the pid of the process to drop, a whole number above zero");
        return NULL;
    }
    return runtime_processes_with_holders(request, release_step, refusal);
}

static cJSON *exit_answer(int exit_now) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddBoolToObject(answer, "exit", exit_now) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *prune_step(const runtime_processes_request *request, const char *path, runtime_holders_record *record,
                         cJSON **refusal) {
    double self = runtime_processes_pid_member(request->argument, "self");
    if (!isnan(record->pid) && record->pid != self) {
        return exit_answer(1);
    }
    runtime_keep_live_holders(record, NAN);
    int unheld = record->count == 0;
    if (unheld) {
        record->pid = NAN;
    } else {
        record->pid = self;
        record->alive_at = floor(runtime_processes_positive_member(request->argument, "now", runtime_now_milliseconds()));
        record->check_milliseconds = runtime_processes_positive_member(request->argument, "checkMilliseconds",
                                                                       RUNTIME_DEFAULT_CHECK_MILLISECONDS);
    }
    if (!runtime_write_holders(path, record)) {
        *refusal = write_refusal(path);
        return NULL;
    }
    return exit_answer(unheld);
}

static cJSON *prune_for(const runtime_processes_request *request, cJSON **refusal) {
    if (isnan(runtime_processes_pid_member(request->argument, "self"))) {
        *refusal = runtime_processes_argument_refusal("self", "the background host's own pid, a whole number above zero");
        return NULL;
    }
    if (isnan(runtime_processes_positive_member(request->argument, "now", 1)) ||
        isnan(runtime_processes_positive_member(request->argument, "checkMilliseconds", 1))) {
        *refusal = runtime_processes_argument_refusal("now or checkMilliseconds", "milliseconds above zero, or leave it out");
        return NULL;
    }
    return runtime_processes_with_holders(request, prune_step, refusal);
}

static int32_t answer_or_refuse(cJSON *answer, cJSON *refusal, uint8_t **out, size_t *out_len) {
    if (answer != NULL || refusal == NULL) {
        return runtime_catalog_answer(answer, out, out_len);
    }
    int32_t status = spisor_fail(cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring,
                                 cJSON_GetObjectItemCaseSensitive(refusal, "fix")->valuestring, out, out_len);
    cJSON_Delete(refusal);
    return status;
}

static int32_t refuse_host_id(const runtime_processes_request *request, runtime_processes_host host, uint8_t **out,
                              size_t *out_len) {
    const char *runs[3] = {request->state, "bundles", "runs"};
    char *folder = host == RUNTIME_PROCESSES_RUNS_HOST ? runtime_cache_joined(runs, 3)
                                                       : runtime_processes_folder(request->state);
    char *problem = NULL;
    const char *recording = host == RUNTIME_PROCESSES_RUNS_HOST ? "runs" : "processes";
    free(folder == NULL ? NULL : runtime_host_file_path(folder, request->host_id, recording, &problem));
    free(folder);
    if (problem == NULL) {
        return runtime_catalog_answer(NULL, out, out_len);
    }
    int32_t status = spisor_fail(problem, RUNTIME_PLAIN_HOST_ID_FIX, out, out_len);
    free(problem);
    return status;
}

static int32_t serve_request(const cJSON *argument, runtime_processes_host host, runtime_processes_operation operation,
                             uint8_t **out, size_t *out_len) {
    runtime_processes_request request = {argument, runtime_catalog_text_member(argument, "state"),
                                         runtime_catalog_text_member(argument, "hostId")};
    if (request.state == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, request.state)) {
        return runtime_catalog_refuse_argument("state", "the host's absolute state folder", out, out_len);
    }
    if (host != RUNTIME_PROCESSES_NO_HOST && request.host_id == NULL) {
        return runtime_catalog_refuse_argument("hostId", "the id of the host the call is for", out, out_len);
    }
    if (host != RUNTIME_PROCESSES_NO_HOST && !runtime_is_plain_host_id(request.host_id)) {
        return refuse_host_id(&request, host, out, out_len);
    }
    cJSON *refusal = NULL;
    cJSON *answer = operation(&request, &refusal);
    return answer_or_refuse(answer, refusal, out, out_len);
}

int32_t runtime_processes_serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                                runtime_processes_host host, runtime_processes_operation operation) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = serve_request(call.argument, host, operation, out, out_len);
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_processes_hold(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_BACKGROUND_HOST,
                                   runtime_processes_hold_for);
}

RUNTIME_EXPORT int32_t runtime_processes_release(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_BACKGROUND_HOST, release_for);
}

RUNTIME_EXPORT int32_t runtime_processes_prune(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_BACKGROUND_HOST, prune_for);
}

RUNTIME_EXPORT int32_t runtime_processes_record_run(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_RUNS_HOST,
                                   runtime_processes_record_run_for);
}

RUNTIME_EXPORT int32_t runtime_processes_live_runs(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_NO_HOST,
                                   runtime_processes_live_runs_for);
}

RUNTIME_EXPORT int32_t runtime_processes_remove_runs(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_RUNS_HOST,
                                   runtime_processes_remove_runs_for);
}
