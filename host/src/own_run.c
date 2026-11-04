#include "own_run.h"

#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "cache_place.h"
#include "own_executable.h"
#include "paths.h"
#include "processes.h"

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

/** The version segment of the executable below cache/bundles/forebay-host, freed with free; NULL outside it. */
static char *own_version(const host_context *context) {
    char *executable = host_own_executable();
    const char *segments[2] = {"bundles", RUNTIME_BOOTED_HOST_ID};
    char *folder = runtime_cache_resolve(context->cache, segments, 2);
    char *resolved = executable == NULL ? NULL : runtime_cache_resolve(executable, NULL, 0);
    char *version = NULL;
    if (folder != NULL && resolved != NULL && runtime_cache_is_strictly_inside(folder, resolved)) {
        const char *start = resolved + strlen(folder) + 1;
        size_t length = 0;
        while (start[length] != 0 && !is_separator(start[length])) {
            length++;
        }
        version = start[length] == 0 ? NULL : malloc(length + 1);
        if (version != NULL) {
            memcpy(version, start, length);
            version[length] = 0;
        }
    }
    free(resolved);
    free(folder);
    free(executable);
    return version;
}

static cJSON *run_call(const own_run *run, const char *version) {
    cJSON *call = cJSON_CreateObject();
    int built = call != NULL && cJSON_AddStringToObject(call, "state", run->context->state) != NULL &&
                cJSON_AddStringToObject(call, "hostId", RUNTIME_BOOTED_HOST_ID) != NULL &&
                cJSON_AddNumberToObject(call, "checkMilliseconds", HOST_OWN_RUN_INTERVAL_MILLISECONDS) != NULL;
    cJSON *ran = built ? cJSON_AddObjectToObject(call, "run") : NULL;
    built = ran != NULL && (version == NULL || cJSON_AddStringToObject(ran, RUNTIME_BOOTED_HOST_ID, version) != NULL) &&
            (run->record == NULL || cJSON_AddStringToObject(call, "record", run->record) != NULL);
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static char *recorded(const own_run *run, const char *version) {
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_processes_record_run, run_call(run, version), &refusal);
    const char *record = host_text(answer, "record");
    char *copy = record == NULL ? NULL : malloc(strlen(record) + 1);
    if (copy != NULL) {
        strcpy(copy, record);
    }
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
    return copy;
}

void host_start_own_run(own_run *run, const host_context *context) {
    run->context = context;
    run->record = NULL;
    run->version = own_version(context);
    run->record = run->version == NULL ? NULL : recorded(run, run->version);
}

void host_stamp_own_run(own_run *run) {
    if (run->record != NULL) {
        free(recorded(run, run->version));
    }
}

void host_finish_own_run(own_run *run) {
    free(run->version);
    run->version = NULL;
    if (run->record == NULL) {
        return;
    }
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "state", run->context->state) == NULL ||
                         cJSON_AddStringToObject(call, "hostId", RUNTIME_BOOTED_HOST_ID) == NULL ||
                         cJSON_AddStringToObject(call, "record", run->record) == NULL)) {
        cJSON_Delete(call);
        call = NULL;
    }
    cJSON *refusal = NULL;
    cJSON_Delete(host_call(runtime_processes_remove_runs, call, &refusal));
    cJSON_Delete(refusal);
    free(run->record);
    run->record = NULL;
}
