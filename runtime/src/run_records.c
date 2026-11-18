#include "run_records.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "alive_stamp.h"
#include "cache_folders.h"
#include "directory_tree.h"
#include "json_service.h"
#include "file_times.h"
#include "host_references.h"
#include "node_json_file.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/javascript.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

static volatile long record_number = 0;

static unsigned long next_record_number(void) {
#if defined(_WIN32)
    return (unsigned long)InterlockedIncrement(&record_number);
#else
    return (unsigned long)__atomic_add_fetch(&record_number, 1, __ATOMIC_RELAXED);
#endif
}

static char *runs_folder(const char *state, const char *host_id) {
    if (!runtime_is_plain_host_id(host_id)) {
        return NULL;
    }
    const char *segments[4] = {state, "bundles", "runs", host_id};
    return runtime_cache_joined(segments, 4);
}

static char *record_text(const cJSON *run, double check_milliseconds) {
    cJSON *record = cJSON_CreateObject();
    int built = record != NULL && cJSON_AddNumberToObject(record, "pid", (double)current_process_id()) != NULL &&
                cJSON_AddNumberToObject(record, "aliveAt", floor(runtime_now_milliseconds())) != NULL &&
                cJSON_AddNumberToObject(record, "checkMilliseconds", check_milliseconds) != NULL &&
                runtime_add_reference(record, "run", run);
    char *compact = built ? cJSON_PrintUnformatted(record) : NULL;
    cJSON_Delete(record);
    char *text = compact == NULL ? NULL : spisor_memory_allocate(strlen(compact) + 2);
    if (text != NULL) {
        snprintf(text, strlen(compact) + 2, "%s\n", compact);
    }
    cJSON_free(compact);
    return text;
}

static char *record_path(const char *folder) {
    char name[64];
    snprintf(name, sizeof name, "%d.%.0f-%lu.json", (int)current_process_id(), floor(runtime_now_milliseconds()),
             next_record_number());
    const char *segments[2] = {folder, name};
    return runtime_cache_joined(segments, 2);
}

/** Writes the record; 0 with *out_of_memory set to 1 when memory ran out building its text. */
static int write_record(const char *path, const cJSON *run, double check_milliseconds, int *out_of_memory) {
    char *text = record_text(run, check_milliseconds);
    spisor_status status = text == NULL ? SPISOR_OUT_OF_MEMORY
                                        : spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    *out_of_memory = status == SPISOR_OUT_OF_MEMORY;
    spisor_memory_release(text);
    return status == SPISOR_OK;
}

char *runtime_record_run(const char *state, const char *host_id, const cJSON *run, double check_milliseconds,
                         spisor_prepared_path *removal, int *out_of_memory) {
    char *folder = runs_folder(state, host_id);
    spisor_status made = folder == NULL ? SPISOR_REFUSED : spisor_make_directories(folder);
    char *path = made == SPISOR_OK ? record_path(folder) : NULL;
    spisor_status prepared = path == NULL ? SPISOR_REFUSED : spisor_prepare_path(path, removal);
    *out_of_memory = (folder == NULL && runtime_is_plain_host_id(host_id)) || made == SPISOR_OUT_OF_MEMORY ||
                     (made == SPISOR_OK && path == NULL) || prepared == SPISOR_OUT_OF_MEMORY;
    int written = prepared == SPISOR_OK && write_record(path, run, check_milliseconds, out_of_memory);
    spisor_memory_release(folder);
    if (!written) {
        if (prepared == SPISOR_OK) {
            spisor_release_prepared_path(removal);
        }
        spisor_memory_release(path);
        return NULL;
    }
    return path;
}

static int is_record_name(const char *name) {
    size_t length = strlen(name);
    return length > strlen(RUNTIME_JSON_EXTENSION) && strpbrk(name, "/\\") == NULL &&
           strcmp(name + length - strlen(RUNTIME_JSON_EXTENSION), RUNTIME_JSON_EXTENSION) == 0;
}

/** @implNote Compared after both are joined the same way, so the path answered earlier matches as given. */
int runtime_is_run_record_of(const char *state, const char *host_id, const char *record, int *out_of_memory) {
    int absolute = record != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, record);
    char *folder = runs_folder(state, host_id);
    char *normal = absolute ? runtime_cache_joined(&record, 1) : NULL;
    size_t length = folder == NULL ? 0 : strlen(folder);
    int inside = folder != NULL && normal != NULL && strncmp(normal, folder, length) == 0 &&
                 (normal[length] == '/' || normal[length] == '\\') && is_record_name(normal + length + 1);
    *out_of_memory = (folder == NULL && runtime_is_plain_host_id(host_id)) || (absolute && normal == NULL);
    spisor_memory_release(normal);
    spisor_memory_release(folder);
    return inside;
}

runtime_run_stamp runtime_stamp_run(const char *state, const char *host_id, const char *record, const cJSON *run,
                                    double check_milliseconds) {
    int out_of_memory = 0;
    if (!runtime_is_run_record_of(state, host_id, record, &out_of_memory)) {
        return out_of_memory ? RUNTIME_RUN_OUT_OF_MEMORY : RUNTIME_RUN_NOT_A_RECORD;
    }
    char *folder = runs_folder(state, host_id);
    spisor_status made = folder == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(folder);
    out_of_memory = made == SPISOR_OUT_OF_MEMORY;
    int stamped = made == SPISOR_OK && write_record(record, run, check_milliseconds, &out_of_memory);
    spisor_memory_release(folder);
    if (folder == NULL || out_of_memory) {
        return RUNTIME_RUN_OUT_OF_MEMORY;
    }
    return stamped ? RUNTIME_RUN_STAMPED : RUNTIME_RUN_UNWRITABLE;
}

static int is_live_record(const cJSON *record, double now) {
    return cJSON_IsObject(record) && runtime_is_version_map(cJSON_GetObjectItemCaseSensitive(record, "run")) &&
           runtime_is_stamp_fresh(runtime_number_or_nan(record, "pid"), runtime_number_or_nan(record, "aliveAt"),
                                  runtime_number_or_nan(record, "checkMilliseconds"), now);
}

/** Adds what the record at path runs when it is live; deletes it otherwise. 0 when memory runs out. */
static int add_live_run(cJSON *runs, const char *path, double now) {
    size_t length = 0;
    spisor_read_outcome outcome;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    if (bytes == NULL) {
        return outcome != SPISOR_READ_OUT_OF_MEMORY;
    }
    int out_of_memory;
    cJSON *record = runtime_parse_node_json(bytes, length, &out_of_memory);
    spisor_memory_release(bytes);
    int added = !out_of_memory;
    if (added && is_live_record(record, now)) {
        cJSON *run = cJSON_DetachItemFromObjectCaseSensitive(record, "run");
        added = run != NULL && cJSON_AddItemToArray(runs, run);
        if (!added) {
            cJSON_Delete(run);
        }
    } else if (added) {
        added = runtime_remove_file_entry(path) != RUNTIME_ENTRY_OUT_OF_MEMORY;
    }
    cJSON_Delete(record);
    return added;
}

static int add_host_runs(cJSON *runs, const char *state, const char *host, double now) {
    const char *segments[4] = {state, "bundles", "runs", host};
    cJSON *names = runtime_cache_json_names(segments, 4);
    int added = names != NULL;
    for (const cJSON *name = added ? names->child : NULL; added && name != NULL; name = name->next) {
        const char *file_segments[5] = {state, "bundles", "runs", host, name->valuestring};
        char *path = runtime_cache_joined(file_segments, 5);
        added = path != NULL && add_live_run(runs, path, now);
        spisor_memory_release(path);
    }
    cJSON_Delete(names);
    return added;
}

cJSON *runtime_live_runs(const char *state, double now) {
    const char *segments[3] = {state, "bundles", "runs"};
    cJSON *hosts = runtime_cache_folder_names(segments, 3);
    cJSON *runs = hosts == NULL ? NULL : cJSON_CreateArray();
    int added = runs != NULL;
    for (const cJSON *host = added ? hosts->child : NULL; added && host != NULL; host = host->next) {
        added = add_host_runs(runs, state, host->valuestring, now);
    }
    cJSON_Delete(hosts);
    if (!added) {
        cJSON_Delete(runs);
        return NULL;
    }
    return runs;
}

spisor_status runtime_remove_run_records(const char *state, const char *host_id) {
    char *folder = runs_folder(state, host_id);
    spisor_status removed = folder == NULL ? SPISOR_OUT_OF_MEMORY : runtime_remove_tree(folder);
    spisor_memory_release(folder);
    return removed;
}

spisor_status runtime_remove_run_record(const char *state, const char *host_id, const char *record) {
    int out_of_memory = 0;
    if (!runtime_is_run_record_of(state, host_id, record, &out_of_memory)) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
    }
    runtime_entry_outcome removed = runtime_remove_file_entry(record);
    return removed == RUNTIME_ENTRY_DONE          ? SPISOR_OK
           : removed == RUNTIME_ENTRY_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY
                                                    : SPISOR_REFUSED;
}
