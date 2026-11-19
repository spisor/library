#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "processes_ready.h"
#include "allocation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "json_service.h"
#include "node_json_file.h"
#include "processes_holders.h"
#include "spisor/status.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define READY_EXTENSION ".ready.json"
#define POLL_MILLISECONDS 50
#define NOT_READY_FIX "try the launch again; if the session host keeps failing to start, turn off the plugins the settings enable"

static void pause_milliseconds(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

static char *ready_path(const char *state, const char *host_id) {
    char *problem = NULL;
    char *path = runtime_holders_path(state, host_id, READY_EXTENSION, &problem);
    spisor_memory_release(problem);
    return path;
}

static int add_text(cJSON *record, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(record, name, text) != NULL;
}

static int add_environment(cJSON *record, const runtime_processes_publication *publication) {
    if (publication->refused != NULL) {
        return 1;
    }
    cJSON *environment = publication->environment == NULL ? cJSON_CreateObject()
                                                          : cJSON_Duplicate(publication->environment, 1);
    return runtime_add_owned(record, "environment", environment);
}

static cJSON *record_of(const runtime_processes_publication *publication) {
    cJSON *record = cJSON_CreateObject();
    int built = record != NULL && cJSON_AddNumberToObject(record, "pid", publication->pid) != NULL &&
                add_text(record, "refused", publication->refused) && add_text(record, "fix", publication->fix) &&
                add_environment(record, publication);
    if (!built) {
        cJSON_Delete(record);
        return NULL;
    }
    return record;
}

/** Writes the record at path; REFUSED when it cannot be written, OUT_OF_MEMORY having written nothing. */
static spisor_status write_record(const char *state, const char *path, const cJSON *record) {
    char *text = cJSON_PrintUnformatted(record);
    char *folder = text == NULL ? NULL : runtime_processes_folder(state);
    spisor_status status = folder == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(folder);
    if (status == SPISOR_OK) {
        status = spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    }
    spisor_memory_release(folder);
    cJSON_free(text);
    return status;
}

int runtime_processes_ready(const runtime_processes_publication *publication, cJSON **refusal) {
    cJSON *record = record_of(publication);
    char *path = record == NULL ? NULL : ready_path(publication->state, publication->host_id);
    spisor_status written = path == NULL ? SPISOR_OUT_OF_MEMORY : write_record(publication->state, path, record);
    *refusal = written == SPISOR_REFUSED ? runtime_processes_state_refusal("the ready record", path) : NULL;
    spisor_memory_release(path);
    cJSON_Delete(record);
    return written == SPISOR_OK;
}

int runtime_publish_ready_refusal(const char *state, const char *host_id, const cJSON *refusal) {
    runtime_processes_publication publication = {
        state,
        host_id,
        (double)current_process_id(),
        NULL,
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refusal, "message")),
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refusal, "fix")),
    };
    cJSON *publication_refused = NULL;
    int published = publication.refused != NULL && publication.fix != NULL &&
                    runtime_processes_ready(&publication, &publication_refused);
    cJSON_Delete(publication_refused);
    return published;
}

static int is_record_of(const cJSON *record, double pid) {
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(record, "pid");
    return cJSON_IsObject(record) && cJSON_IsNumber(recorded) && recorded->valuedouble == pid;
}

static cJSON *environment_answer(const cJSON *record) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(record, "environment");
    cJSON *answer = cJSON_CreateObject();
    cJSON *copy = runtime_processes_is_environment(environment) ? cJSON_Duplicate(environment, 1) : cJSON_CreateObject();
    if (!runtime_add_owned(answer, "environment", copy)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *published_refusal(const cJSON *record) {
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(record, "fix");
    const char *parts[1] = {cJSON_GetObjectItemCaseSensitive(record, "refused")->valuestring};
    return runtime_cache_refusal(parts, 1, cJSON_IsString(fix) ? fix->valuestring : NOT_READY_FIX);
}

static cJSON *timeout_refusal(const char *host_id, double timeout_milliseconds) {
    char seconds[32];
    snprintf(seconds, sizeof seconds, "%g", timeout_milliseconds / 1000.0);
    const char *parts[5] = {"the session host of ", host_id, " did not become ready within ", seconds, " seconds"};
    return runtime_cache_refusal(parts, 5, NOT_READY_FIX);
}

static int is_refusal_record(const cJSON *record) {
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(record, "refused"));
}

cJSON *runtime_processes_await_ready(const char *state, const char *host_id, double pid, double timeout,
                                     cJSON **refusal) {
    *refusal = NULL;
    char *path = ready_path(state, host_id);
    double started = runtime_now_milliseconds();
    cJSON *answer = NULL;
    int settled = 0;
    int out_of_memory = path == NULL;
    while (!out_of_memory && !settled) {
        cJSON *record = runtime_read_node_json(path, &out_of_memory);
        settled = out_of_memory || is_record_of(record, pid);
        if (!out_of_memory && settled && is_refusal_record(record)) {
            *refusal = published_refusal(record);
        } else if (!out_of_memory && settled) {
            answer = environment_answer(record);
        }
        cJSON_Delete(record);
        if (!settled && runtime_now_milliseconds() - started >= timeout) {
            *refusal = timeout_refusal(host_id, timeout);
            settled = 1;
        } else if (!settled) {
            pause_milliseconds(POLL_MILLISECONDS);
        }
    }
    spisor_memory_release(path);
    return answer;
}
