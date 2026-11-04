#include "run_records.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "directory_tree.h"
#include "file_times.h"
#include "host_references.h"
#include "spisor/file.h"

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

char *runtime_record_run(const char *state, const char *host_id, const cJSON *run, double check_milliseconds) {
    char *folder = runs_folder(state, host_id);
    char *path = folder != NULL && spisor_make_directories(folder) ? record_path(folder) : NULL;
    char *text = path == NULL ? NULL : record_text(run, check_milliseconds);
    int written = text != NULL && spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    free(text);
    free(folder);
    if (!written) {
        free(path);
        return NULL;
    }
    return path;
}

int runtime_remove_run_records(const char *state, const char *host_id) {
    char *folder = runs_folder(state, host_id);
    int removed = folder != NULL && runtime_remove_tree(folder);
    free(folder);
    return removed;
}
