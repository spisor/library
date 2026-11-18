#include "processes.h"
#include "allocation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "holders_lock.h"
#include "host_references.h"
#include "json_service.h"
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
    spisor_memory_release(fix_text);
    return refusal;
}

cJSON *runtime_processes_state_refusal(const char *what, const char *path) {
    const char *parts[4] = {what, " at ", path, " could not be used"};
    return runtime_cache_refusal(parts, 4, WRITABLE_STATE_FIX);
}

static int is_named_earlier(const cJSON *environment, const cJSON *variable) {
    for (const cJSON *earlier = environment->child; earlier != variable; earlier = earlier->next) {
        if (strcmp(earlier->string, variable->string) == 0) {
            return 1;
        }
    }
    return 0;
}

int runtime_processes_is_environment(const cJSON *environment) {
    if (!cJSON_IsObject(environment)) {
        return 0;
    }
    for (const cJSON *variable = environment->child; variable != NULL; variable = variable->next) {
        if (!cJSON_IsString(variable) || *variable->string == 0 || strchr(variable->string, '=') != NULL ||
            is_named_earlier(environment, variable)) {
            return 0;
        }
    }
    return 1;
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

static cJSON *lock_refusal(const char *lock) {
    const char *message[3] = {"the holders lock at ", lock, " could not be taken"};
    const char *fix[3] = {"remove whatever stands at ", lock,
                          " when it is no lock file, check that SPISOR_STATE names a writable folder, then try again"};
    char *fix_text = runtime_cache_text(fix, 3);
    cJSON *refusal = fix_text == NULL ? NULL : runtime_cache_refusal(message, 3, fix_text);
    spisor_memory_release(fix_text);
    return refusal;
}

static cJSON *locked_step(const runtime_processes_request *request, const char *path, runtime_holders_step step,
                          cJSON **refusal) {
    char *problem = NULL;
    char *lock = runtime_holders_path(request->state, request->host_id, ".lock", &problem);
    runtime_holders_guard *guard = NULL;
    spisor_status locked = lock == NULL ? SPISOR_OUT_OF_MEMORY : runtime_lock_holders(lock, &guard);
    if (locked != SPISOR_OK) {
        *refusal = locked == SPISOR_OUT_OF_MEMORY ? NULL : lock_refusal(lock);
        spisor_memory_release(lock);
        spisor_memory_release(problem);
        return NULL;
    }
    runtime_holders_record record = runtime_no_holders();
    cJSON *answer = NULL;
    if (runtime_read_holders(path, &record)) {
        answer = step(request, path, &record, refusal);
    }
    runtime_free_holders(&record);
    runtime_unlock_holders(guard);
    spisor_memory_release(lock);
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
    spisor_memory_release(folder);
    spisor_memory_release(path);
    spisor_memory_release(problem);
    return answer;
}

cJSON *runtime_processes_write_answered(const char *path, const runtime_holders_record *record, cJSON *answer,
                                        cJSON **refusal) {
    spisor_status written = answer == NULL ? SPISOR_OUT_OF_MEMORY : runtime_write_holders(path, record);
    if (written != SPISOR_OK) {
        *refusal = written == SPISOR_REFUSED ? runtime_processes_state_refusal("the holders file", path) : NULL;
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *release_step(const runtime_processes_request *request, const char *path, runtime_holders_record *record,
                           cJSON **refusal) {
    runtime_keep_live_holders(record, runtime_processes_pid_member(request->argument, "holder"));
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL &&
        !runtime_catalog_attach(answer, "holders", runtime_processes_holders_array(record->holders, record->count))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return runtime_processes_write_answered(path, record, answer, refusal);
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
    return runtime_processes_write_answered(path, record, exit_answer(unheld), refusal);
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

static cJSON *refuse_host_id(const runtime_processes_request *request, runtime_processes_host host, cJSON **refusal) {
    const char *runs[3] = {request->state, "bundles", "runs"};
    char *folder = host == RUNTIME_PROCESSES_RUNS_HOST ? runtime_cache_joined(runs, 3)
                                                       : runtime_processes_folder(request->state);
    char *problem = NULL;
    const char *recording = host == RUNTIME_PROCESSES_RUNS_HOST ? "runs" : "processes";
    char *host_file = folder == NULL ? NULL : runtime_host_file_path(folder, request->host_id, recording, &problem);
    spisor_memory_release(host_file);
    spisor_memory_release(folder);
    *refusal = problem == NULL ? NULL : runtime_refusal(problem, RUNTIME_PLAIN_HOST_ID_FIX);
    spisor_memory_release(problem);
    return NULL;
}

cJSON *runtime_processes_serve(const cJSON *argument, runtime_processes_host host,
                               runtime_processes_operation operation, cJSON **refusal) {
    runtime_processes_request request = {argument, runtime_catalog_text_member(argument, "state"),
                                         runtime_catalog_text_member(argument, "hostId")};
    if (request.state == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, request.state)) {
        return runtime_catalog_refuse_member("state", "the host's absolute state folder", refusal);
    }
    if (host != RUNTIME_PROCESSES_NO_HOST && request.host_id == NULL) {
        return runtime_catalog_refuse_member("hostId", "the id of the host the call is for", refusal);
    }
    if (host != RUNTIME_PROCESSES_NO_HOST && !runtime_is_plain_host_id(request.host_id)) {
        return refuse_host_id(&request, host, refusal);
    }
    return operation(&request, refusal);
}

cJSON *runtime_processes_hold_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_BACKGROUND_HOST, runtime_processes_hold_for, refusal);
}

cJSON *runtime_processes_release_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_BACKGROUND_HOST, release_for, refusal);
}

cJSON *runtime_processes_prune_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_BACKGROUND_HOST, prune_for, refusal);
}

cJSON *runtime_processes_record_run_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_RUNS_HOST, runtime_processes_record_run_for, refusal);
}

cJSON *runtime_processes_live_runs_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_NO_HOST, runtime_processes_live_runs_for, refusal);
}

cJSON *runtime_processes_remove_runs_operation(const cJSON *argument, cJSON **refusal) {
    return runtime_processes_serve(argument, RUNTIME_PROCESSES_RUNS_HOST, runtime_processes_remove_runs_for, refusal);
}
