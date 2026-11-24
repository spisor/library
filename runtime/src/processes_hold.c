#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "alive_stamp.h"
#include "allocation.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "paths.h"
#include "process_start.h"
#include "json_service.h"
#include "processes.h"
#include "processes_holders.h"
#include "spisor/path.h"

#define START_FIX "check the start the host's registration records names an executable that exists, then try again"

/** The start the request names, its directory defaulted to state; NULL directory when the request's is not absolute. */
static runtime_detached_start start_of(const runtime_processes_hold_request *request) {
    runtime_detached_start start = request->start;
    if (start.directory == NULL) {
        start.directory = request->state;
    } else if (!spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, start.directory)) {
        start.directory = NULL;
    }
    return start;
}

static int is_host_running(const runtime_holders_record *record) {
    return runtime_is_stamp_fresh(record->pid, record->alive_at, record->check_milliseconds, runtime_now_milliseconds());
}

static cJSON *held_answer(double pid, int started, int outside_job) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddNumberToObject(answer, "pid", pid) != NULL &&
                cJSON_AddBoolToObject(answer, "started", started) != NULL &&
                (!started || cJSON_AddBoolToObject(answer, "outsideJob", outside_job) != NULL);
    if (!built) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *start_refusal(char *problem) {
    const char *parts[1] = {problem};
    cJSON *refusal = problem == NULL ? NULL : runtime_cache_refusal(parts, 1, START_FIX);
    spisor_memory_release(problem);
    return refusal;
}

/** Fills the started host into the answer and the draft, and records it: REFUSED when it could not be written. */
static spisor_status record_started(const char *path, runtime_holders_draft *draft, cJSON *answer, double pid,
                                    int outside_job) {
    (void)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(answer, "pid"), pid);
    (void)cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(answer, "outsideJob"), outside_job);
    if (!runtime_print_started_host(draft, pid, floor(runtime_now_milliseconds()))) {
        return SPISOR_REFUSED;
    }
    return runtime_write_holders_draft(path, draft);
}

/**
 * Rewrites the record without this call's holder, appended last, once the host it recorded never ran its program,
 * so the record names no holder of a host that does not run.
 */
static void unrecord(const char *path, runtime_holders_record *record) {
    record->count--;
    (void)runtime_write_holders(path, record);
}

/** Runs the recorded host: the answer, or NULL with *refusal set when its program could not run. */
static cJSON *run_recorded(runtime_detached_host *host, const char *path, runtime_holders_record *record,
                           cJSON *answer, cJSON **refusal) {
    char *problem = NULL;
    if (runtime_release_detached(host, &problem)) {
        return answer;
    }
    unrecord(path, record);
    *refusal = start_refusal(problem);
    cJSON_Delete(answer);
    return NULL;
}

/**
 * Starts the host and records it as the holders' host.
 *
 * @implNote The answer and the record's text are built before the start, and the host is held before its program
 * runs until it is recorded, so a record that cannot be written ends a host that never ran, through the handle or
 * pipe the start holds rather than by a pid another process may have taken since.
 */
static cJSON *start_host(const runtime_processes_hold_request *request, const char *path,
                         runtime_holders_record *record, cJSON **refusal) {
    runtime_holders_draft draft;
    cJSON *answer = held_answer(0, 1, 1);
    if (answer == NULL || !runtime_draft_started_host(record, &draft)) {
        cJSON_Delete(answer);
        return NULL;
    }
    runtime_detached_start start = start_of(request);
    runtime_detached_host host;
    char *problem = NULL;
    spisor_status recorded = SPISOR_REFUSED;
    if (!runtime_start_detached(&start, &host, &problem)) {
        *refusal = start_refusal(problem);
        cJSON_Delete(answer);
        answer = NULL;
    } else if ((recorded = record_started(path, &draft, answer, host.pid, host.outside_job)) != SPISOR_OK) {
        runtime_cancel_detached(&host);
        *refusal = recorded == SPISOR_OUT_OF_MEMORY ? NULL : runtime_processes_state_refusal("the holders file", path);
        cJSON_Delete(answer);
        answer = NULL;
    } else {
        answer = run_recorded(&host, path, record, answer, refusal);
    }
    runtime_free_holders_draft(&draft);
    return answer;
}

static cJSON *hold_step(const void *context, const char *path, runtime_holders_record *record, cJSON **refusal) {
    const runtime_processes_hold_request *request = context;
    runtime_keep_live_holders(record, request->holder);
    if (!runtime_add_holder(record, request->holder)) {
        return NULL;
    }
    if (!is_host_running(record)) {
        return start_host(request, path, record, refusal);
    }
    return runtime_processes_write_answered(path, record, held_answer(record->pid, 0, 1), refusal);
}

cJSON *runtime_processes_hold(const runtime_processes_hold_request *request, cJSON **refusal) {
    *refusal = NULL;
    if (start_of(request).directory == NULL) {
        *refusal = runtime_refusal("the call carries no usable start directory", "pass an absolute folder, or none");
        return NULL;
    }
    return runtime_processes_with_holders(request->state, request->host_id, runtime_now_milliseconds(), hold_step,
                                          request, refusal);
}
