#include "run_records.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alive_stamp.h"
#include "cache_folders.h"
#include "directory_tree.h"
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
                cJSON_AddItemReferenceToObject(record, "run", (cJSON *)run);
    char *compact = built ? cJSON_PrintUnformatted(record) : NULL;
    cJSON_Delete(record);
    char *text = compact == NULL ? NULL : malloc(strlen(compact) + 2);
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

static int write_record(const char *path, const cJSON *run, double check_milliseconds) {
    char *text = record_text(run, check_milliseconds);
    int written = text != NULL && spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    free(text);
    return written;
}

char *runtime_record_run(const char *state, const char *host_id, const cJSON *run, double check_milliseconds) {
    char *folder = runs_folder(state, host_id);
    char *path = folder != NULL && spisor_make_directories(folder) ? record_path(folder) : NULL;
    int written = path != NULL && write_record(path, run, check_milliseconds);
    free(folder);
    if (!written) {
        free(path);
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
int runtime_is_run_record_of(const char *state, const char *host_id, const char *record) {
    char *folder = runs_folder(state, host_id);
    char *normal = record == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, record)
                       ? NULL
                       : runtime_cache_joined(&record, 1);
    size_t length = folder == NULL ? 0 : strlen(folder);
    int inside = folder != NULL && normal != NULL && strncmp(normal, folder, length) == 0 &&
                 (normal[length] == '/' || normal[length] == '\\') && is_record_name(normal + length + 1);
    free(normal);
    free(folder);
    return inside;
}

runtime_run_stamp runtime_stamp_run(const char *state, const char *host_id, const char *record, const cJSON *run,
                                    double check_milliseconds) {
    if (!runtime_is_run_record_of(state, host_id, record)) {
        return RUNTIME_RUN_NOT_A_RECORD;
    }
    char *folder = runs_folder(state, host_id);
    int stamped = folder != NULL && spisor_make_directories(folder) && write_record(record, run, check_milliseconds);
    free(folder);
    return stamped ? RUNTIME_RUN_STAMPED : RUNTIME_RUN_UNWRITABLE;
}

static cJSON *parsed_record(const uint8_t *bytes, size_t length) {
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, length, &decoded_length);
    cJSON *record = decoded == NULL ? NULL : spisor_parse_json(decoded, decoded_length);
    free(decoded);
    return record;
}

static int is_live_record(const cJSON *record, double now) {
    return cJSON_IsObject(record) && runtime_is_version_map(cJSON_GetObjectItemCaseSensitive(record, "run")) &&
           runtime_is_stamp_fresh(runtime_number_or_nan(record, "pid"), runtime_number_or_nan(record, "aliveAt"),
                                  runtime_number_or_nan(record, "checkMilliseconds"), now);
}

/** Adds what the record at path runs when it is live; deletes it otherwise. 0 when memory runs out. */
static int add_live_run(cJSON *runs, const char *path, double now) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    if (bytes == NULL) {
        return 1;
    }
    cJSON *record = parsed_record(bytes, length);
    free(bytes);
    int added = 1;
    if (is_live_record(record, now)) {
        cJSON *run = cJSON_DetachItemFromObjectCaseSensitive(record, "run");
        added = run != NULL && cJSON_AddItemToArray(runs, run);
        if (!added) {
            cJSON_Delete(run);
        }
    } else {
        runtime_remove_file_entry(path);
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
        free(path);
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

int runtime_remove_run_records(const char *state, const char *host_id) {
    char *folder = runs_folder(state, host_id);
    int removed = folder != NULL && runtime_remove_tree(folder);
    free(folder);
    return removed;
}

int runtime_remove_run_record(const char *state, const char *host_id, const char *record) {
    return runtime_is_run_record_of(state, host_id, record) && runtime_remove_file_entry(record) == RUNTIME_ENTRY_DONE;
}
