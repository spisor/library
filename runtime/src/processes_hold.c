#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "alive_stamp.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "paths.h"
#include "process_start.h"
#include "processes_request.h"
#include "spisor/path.h"

#define START_FIX "check the start the host's registration records names an executable that exists, then try again"

static int is_text_list(const cJSON *list) {
    if (!cJSON_IsArray(list)) {
        return 0;
    }
    for (const cJSON *item = list->child; item != NULL; item = item->next) {
        if (!cJSON_IsString(item)) {
            return 0;
        }
    }
    return 1;
}

static const char *absolute_text(const cJSON *object, const char *name) {
    const char *text = runtime_catalog_text_member(object, name);
    return text != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, text) ? text : NULL;
}

/** Reads the start member into *start, answering the refusal of a member that is not usable, or NULL. */
static cJSON *read_start(const runtime_processes_request *request, runtime_detached_start *start) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(request->argument, "start");
    start->executable = absolute_text(member, "executable");
    start->arguments = cJSON_GetObjectItemCaseSensitive(member, "arguments");
    start->environment = cJSON_GetObjectItemCaseSensitive(member, "environment");
    start->directory = cJSON_GetObjectItemCaseSensitive(member, "directory") == NULL ? request->state
                                                                                      : absolute_text(member, "directory");
    if (start->executable == NULL) {
        return runtime_processes_argument_refusal("start.executable", "the absolute path of the program that runs the host");
    }
    if (start->arguments != NULL && !is_text_list(start->arguments)) {
        return runtime_processes_argument_refusal("start.arguments", "an array of strings, or leave it out");
    }
    if (start->environment != NULL && !runtime_processes_is_environment(start->environment)) {
        return runtime_processes_argument_refusal("start.environment",
                                                  "an object of strings whose names hold no = and appear once, "
                                                  "or leave it out");
    }
    if (start->directory == NULL) {
        return runtime_processes_argument_refusal("start.directory", "an absolute folder, or leave it out");
    }
    return NULL;
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
    free(problem);
    return refusal;
}

/** Fills the started host into the answer and the draft, and records it; 0 when the record could not be written. */
static int record_started(const char *path, runtime_holders_draft *draft, cJSON *answer, double pid,
                          int outside_job) {
    (void)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(answer, "pid"), pid);
    (void)cJSON_SetBoolValue(cJSON_GetObjectItemCaseSensitive(answer, "outsideJob"), outside_job);
    return runtime_print_started_host(draft, pid, floor(runtime_now_milliseconds())) &&
           runtime_write_holders_draft(path, draft) == SPISOR_OK;
}

/**
 * Starts the host and records it as the holders' host.
 *
 * @implNote The answer and the record's text are built before the start, so once the host runs nothing is left to
 * allocate but the file replacement; a host whose record still cannot be written is stopped, since a host nobody
 * recorded would be started a second time by the next holder.
 */
static cJSON *start_host(const runtime_processes_request *request, const char *path,
                         const runtime_holders_record *record, cJSON **refusal) {
    runtime_holders_draft draft;
    cJSON *answer = held_answer(0, 1, 1);
    if (answer == NULL || !runtime_draft_started_host(record, &draft)) {
        cJSON_Delete(answer);
        return NULL;
    }
    runtime_detached_start start;
    read_start(request, &start);
    double pid = NAN;
    int outside_job = 1;
    char *problem = NULL;
    if (!runtime_start_detached(&start, &pid, &outside_job, &problem)) {
        *refusal = start_refusal(problem);
        cJSON_Delete(answer);
        answer = NULL;
    } else if (!record_started(path, &draft, answer, pid, outside_job)) {
        runtime_stop_detached(pid);
        *refusal = runtime_processes_state_refusal("the holders file", path);
        cJSON_Delete(answer);
        answer = NULL;
    }
    runtime_free_holders_draft(&draft);
    return answer;
}

static cJSON *hold_step(const runtime_processes_request *request, const char *path, runtime_holders_record *record,
                        cJSON **refusal) {
    double holder = runtime_processes_pid_member(request->argument, "holder");
    runtime_keep_live_holders(record, holder);
    if (!runtime_add_holder(record, holder)) {
        return NULL;
    }
    if (!is_host_running(record)) {
        return start_host(request, path, record, refusal);
    }
    return runtime_processes_write_answered(path, record, held_answer(record->pid, 0, 1), refusal);
}

cJSON *runtime_processes_hold_for(const runtime_processes_request *request, cJSON **refusal) {
    if (isnan(runtime_processes_pid_member(request->argument, "holder"))) {
        *refusal = runtime_processes_argument_refusal("holder", "the pid of the holding process, a whole number above zero");
        return NULL;
    }
    runtime_detached_start start;
    *refusal = read_start(request, &start);
    return *refusal != NULL ? NULL : runtime_processes_with_holders(request, hold_step, refusal);
}
