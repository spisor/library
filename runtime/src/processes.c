#include "processes.h"
#include "allocation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "holders_lock.h"
#include "json_service.h"
#include "paths.h"
#include "process_alive.h"
#include "processes_holders.h"
#include "run_records.h"
#include "spisor/file.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"

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

static cJSON *locked_step(const char *state, const char *host_id, const char *path, runtime_holders_step step,
                          const void *context, cJSON **refusal) {
    char *problem = NULL;
    char *lock = runtime_holders_path(state, host_id, ".lock", &problem);
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
        answer = step(context, path, &record, refusal);
    }
    runtime_free_holders(&record);
    runtime_unlock_holders(guard);
    spisor_memory_release(lock);
    return answer;
}

cJSON *runtime_processes_with_holders(const char *state, const char *host_id, runtime_holders_step step,
                                      const void *context, cJSON **refusal) {
    char *problem = NULL;
    char *path = runtime_holders_path(state, host_id, RUNTIME_JSON_EXTENSION, &problem);
    char *folder = runtime_processes_folder(state);
    cJSON *answer = NULL;
    spisor_status made = path == NULL || folder == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(folder);
    if (made == SPISOR_REFUSED) {
        *refusal = runtime_processes_state_refusal("the processes folder", folder);
    } else if (made == SPISOR_OK) {
        answer = locked_step(state, host_id, path, step, context, refusal);
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

static cJSON *release_step(const void *context, const char *path, runtime_holders_record *record,
                           cJSON **refusal) {
    const double *holder = context;
    runtime_keep_live_holders(record, *holder);
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL &&
        !runtime_add_owned(answer, "holders", runtime_processes_holders_array(record->holders, record->count))) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    return runtime_processes_write_answered(path, record, answer, refusal);
}

cJSON *runtime_processes_release(const char *state, const char *host_id, double holder, cJSON **refusal) {
    *refusal = NULL;
    return runtime_processes_with_holders(state, host_id, release_step, &holder, refusal);
}

static cJSON *exit_answer(int exit_now) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddBoolToObject(answer, "exit", exit_now) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *prune_step(const void *context, const char *path, runtime_holders_record *record, cJSON **refusal) {
    const runtime_processes_prune_request *request = context;
    if (!isnan(record->pid) && record->pid != request->self && runtime_process_is_alive(record->pid)) {
        return exit_answer(1);
    }
    runtime_keep_live_holders(record, NAN);
    int unheld = record->count == 0;
    if (unheld) {
        record->pid = NAN;
    } else {
        record->pid = request->self;
        record->alive_at = floor(request->now == NULL ? runtime_now_milliseconds() : *request->now);
        record->check_milliseconds =
            request->check_milliseconds == 0 ? RUNTIME_DEFAULT_CHECK_MILLISECONDS : request->check_milliseconds;
    }
    return runtime_processes_write_answered(path, record, exit_answer(unheld), refusal);
}

cJSON *runtime_processes_prune(const runtime_processes_prune_request *request, cJSON **refusal) {
    *refusal = NULL;
    return runtime_processes_with_holders(request->state, request->host_id, prune_step, request, refusal);
}
