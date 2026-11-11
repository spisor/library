#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "processes_ready.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_answer.h"
#include "file_times.h"
#include "processes_request.h"
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

static char *ready_path(const runtime_processes_request *request) {
    char *problem = NULL;
    char *path = runtime_holders_path(request->state, request->host_id, READY_EXTENSION, &problem);
    free(problem);
    return path;
}

static int is_text_member(const cJSON *argument, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(argument, name);
    return cJSON_IsString(value) && *value->valuestring != 0;
}

static int is_refusal_published(const cJSON *argument) {
    return cJSON_HasObjectItem(argument, "refused") && is_text_member(argument, "refused") && is_text_member(argument, "fix");
}

/** The refusal of a publication that is neither an environment nor a refusal with its fix, or NULL. */
static cJSON *publication_refusal(const cJSON *argument) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    int refuses = cJSON_HasObjectItem(argument, "refused");
    if (isnan(runtime_processes_pid_member(argument, "pid"))) {
        return runtime_processes_argument_refusal("pid", "the pid of the session host, a whole number above zero");
    }
    if (environment != NULL && !runtime_processes_is_environment(environment)) {
        return runtime_processes_argument_refusal("environment", "an object of strings whose names hold no =, or leave it out");
    }
    if (refuses && environment != NULL) {
        return runtime_processes_argument_refusal("environment", "nothing beside refused, which publishes that the host cannot serve");
    }
    if (refuses != cJSON_HasObjectItem(argument, "fix") || (refuses && !is_refusal_published(argument))) {
        return runtime_processes_argument_refusal("refused and fix", "both as text, or neither");
    }
    return NULL;
}

static int copy_member(cJSON *record, const cJSON *argument, const char *name) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(argument, name);
    return member == NULL || cJSON_AddItemToObject(record, name, cJSON_Duplicate(member, 1));
}

static cJSON *record_of(const cJSON *argument) {
    cJSON *record = cJSON_CreateObject();
    int built = record != NULL && cJSON_AddNumberToObject(record, "pid", runtime_processes_pid_member(argument, "pid")) != NULL &&
                copy_member(record, argument, "refused") && copy_member(record, argument, "fix") &&
                copy_member(record, argument, "environment");
    if (built && !cJSON_HasObjectItem(record, "refused") && !cJSON_HasObjectItem(record, "environment")) {
        built = cJSON_AddObjectToObject(record, "environment") != NULL;
    }
    if (!built) {
        cJSON_Delete(record);
        return NULL;
    }
    return record;
}

static int write_record(const runtime_processes_request *request, const char *path, const cJSON *record) {
    char *text = cJSON_PrintUnformatted(record);
    char *folder = runtime_processes_folder(request->state);
    int written = text != NULL && folder != NULL && spisor_make_directories(folder) &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    free(folder);
    cJSON_free(text);
    return written;
}

static cJSON *ready_for(const runtime_processes_request *request, cJSON **refusal) {
    *refusal = publication_refusal(request->argument);
    cJSON *record = *refusal == NULL ? record_of(request->argument) : NULL;
    char *path = record == NULL ? NULL : ready_path(request);
    cJSON *answer = NULL;
    if (path != NULL && !write_record(request, path, record)) {
        *refusal = runtime_processes_state_refusal("the ready record", path);
    } else if (path != NULL) {
        answer = cJSON_CreateObject();
    }
    free(path);
    cJSON_Delete(record);
    return answer;
}

static cJSON *refusal_publication(const cJSON *refusal) {
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refusal, "message"));
    const char *fix = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(refusal, "fix"));
    cJSON *argument = message == NULL || fix == NULL ? NULL : cJSON_CreateObject();
    int built = argument != NULL && cJSON_AddNumberToObject(argument, "pid", (double)current_process_id()) != NULL &&
                cJSON_AddStringToObject(argument, "refused", message) != NULL &&
                cJSON_AddStringToObject(argument, "fix", fix) != NULL;
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

int runtime_publish_ready_refusal(const char *state, const char *host_id, const cJSON *refusal) {
    cJSON *argument = refusal_publication(refusal);
    runtime_processes_request request = {argument, state, host_id};
    cJSON *publication_refused = NULL;
    cJSON *answer = argument == NULL ? NULL : ready_for(&request, &publication_refused);
    int published = answer != NULL;
    cJSON_Delete(answer);
    cJSON_Delete(publication_refused);
    cJSON_Delete(argument);
    return published;
}

/** The record at path, or NULL when there is none or it is not yet readable whole. */
static cJSON *read_record(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *record = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
    return record;
}

static int is_record_of(const cJSON *record, double pid) {
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(record, "pid");
    return cJSON_IsObject(record) && cJSON_IsNumber(recorded) && recorded->valuedouble == pid;
}

static cJSON *environment_answer(const cJSON *record) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(record, "environment");
    cJSON *answer = cJSON_CreateObject();
    cJSON *copy = runtime_processes_is_environment(environment) ? cJSON_Duplicate(environment, 1) : cJSON_CreateObject();
    if (answer == NULL || !runtime_catalog_attach(answer, "environment", copy)) {
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

static cJSON *await_ready_for(const runtime_processes_request *request, cJSON **refusal) {
    double pid = runtime_processes_pid_member(request->argument, "pid");
    double timeout = runtime_processes_positive_member(request->argument, "timeoutMilliseconds", NAN);
    if (isnan(pid) || isnan(timeout)) {
        *refusal = runtime_processes_argument_refusal("pid and timeoutMilliseconds", "whole numbers above zero");
        return NULL;
    }
    char *path = ready_path(request);
    double started = runtime_now_milliseconds();
    cJSON *answer = NULL;
    int settled = 0;
    while (path != NULL && !settled) {
        cJSON *record = read_record(path);
        settled = is_record_of(record, pid);
        if (settled && is_refusal_record(record)) {
            *refusal = published_refusal(record);
        } else if (settled) {
            answer = environment_answer(record);
        }
        cJSON_Delete(record);
        if (!settled && runtime_now_milliseconds() - started >= timeout) {
            *refusal = timeout_refusal(request->host_id, timeout);
            settled = 1;
        } else if (!settled) {
            pause_milliseconds(POLL_MILLISECONDS);
        }
    }
    free(path);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_processes_ready(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_BACKGROUND_HOST, ready_for);
}

RUNTIME_EXPORT int32_t runtime_processes_await_ready(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_processes_serve(in, in_len, out, out_len, RUNTIME_PROCESSES_BACKGROUND_HOST, await_ready_for);
}
