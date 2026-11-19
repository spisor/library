#include "holders.h"
#include "allocation.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alive_stamp.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "directory_tree.h"
#include "host_references.h"
#include "json_service.h"
#include "node_json_file.h"
#include "process_alive.h"
#include "spisor/file.h"

/** The most characters cJSON prints for one number, its print buffer's size. */
#define NUMBER_TEXT_WIDTH 26
/** The bytes cJSON_PrintPreallocated asks for beyond the text, since it cannot measure a number before printing it. */
#define PREALLOCATED_SLACK 5

runtime_holders_record runtime_no_holders(void) {
    runtime_holders_record none = {NAN, NAN, NAN, NULL, 0};
    return none;
}

void runtime_free_holders(runtime_holders_record *record) {
    spisor_memory_release(record->holders);
    *record = runtime_no_holders();
}

char *runtime_processes_folder(const char *state) {
    const char *segments[2] = {state, "processes"};
    return runtime_cache_joined(segments, 2);
}

char *runtime_holders_path(const char *state, const char *host_id, const char *extension, char **problem) {
    *problem = NULL;
    char *folder = runtime_processes_folder(state);
    char *json = folder == NULL ? NULL : runtime_host_file_path(folder, host_id, "processes", problem);
    spisor_memory_release(folder);
    if (json == NULL || strcmp(extension, RUNTIME_JSON_EXTENSION) == 0) {
        return json;
    }
    const char *parts[2] = {json, extension};
    json[strlen(json) - strlen(RUNTIME_JSON_EXTENSION)] = 0;
    char *path = runtime_cache_text(parts, 2);
    spisor_memory_release(json);
    return path;
}

/** Node's Number.isInteger(value) && value > 0, the shape of a pid. */
static int is_pid(const cJSON *value) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble > 0 &&
           floor(value->valuedouble) == value->valuedouble;
}

static int every_holder_is_pid(const cJSON *holders) {
    for (const cJSON *holder = holders->child; holder != NULL; holder = holder->next) {
        if (!is_pid(holder)) {
            return 0;
        }
    }
    return 1;
}

static int is_holders_record(const cJSON *value) {
    const cJSON *holders = cJSON_GetObjectItemCaseSensitive(value, "holders");
    return cJSON_IsObject(value) && cJSON_IsArray(holders) && every_holder_is_pid(holders);
}

static int copy_record(const cJSON *value, runtime_holders_record *record) {
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(value, "pid");
    const cJSON *holders = cJSON_GetObjectItemCaseSensitive(value, "holders");
    record->pid = is_pid(pid) ? pid->valuedouble : NAN;
    record->alive_at = runtime_number_or_nan(value, "aliveAt");
    record->check_milliseconds = runtime_number_or_nan(value, "checkMilliseconds");
    for (const cJSON *holder = holders->child; holder != NULL; holder = holder->next) {
        if (!runtime_add_holder(record, holder->valuedouble)) {
            return 0;
        }
    }
    return 1;
}

int runtime_read_holders(const char *path, runtime_holders_record *record) {
    *record = runtime_no_holders();
    int out_of_memory;
    cJSON *value = runtime_read_node_json(path, &out_of_memory);
    int read = !out_of_memory && (!is_holders_record(value) || copy_record(value, record));
    cJSON_Delete(value);
    if (!read) {
        runtime_free_holders(record);
    }
    return read;
}

static cJSON *number_or_null(double value) {
    return isnan(value) ? cJSON_CreateNull() : cJSON_CreateNumber(value);
}

static cJSON *record_tree(const runtime_holders_record *record) {
    cJSON *object = cJSON_CreateObject();
    cJSON *holders = cJSON_CreateArray();
    int built = object != NULL && runtime_add_owned(object, "pid", number_or_null(record->pid)) &&
                runtime_add_owned(object, "aliveAt", number_or_null(record->alive_at)) &&
                runtime_add_owned(object, "checkMilliseconds", number_or_null(record->check_milliseconds));
    if (built) {
        built = runtime_add_owned(object, "holders", holders);
    } else {
        cJSON_Delete(holders);
    }
    for (size_t index = 0; built && index < record->count; index++) {
        cJSON *holder = cJSON_CreateNumber(record->holders[index]);
        built = holder != NULL && cJSON_AddItemToArray(holders, holder);
        if (!built) {
            cJSON_Delete(holder);
        }
    }
    if (!built) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static char *record_text(const runtime_holders_record *record) {
    cJSON *object = record_tree(record);
    char *compact = object == NULL ? NULL : cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    const char *parts[2] = {compact, "\n"};
    char *text = compact == NULL ? NULL : runtime_cache_text(parts, 2);
    cJSON_free(compact);
    return text;
}

static spisor_status make_parent(const char *path) {
    char *parent = spisor_memory_allocate(strlen(path) + 1);
    if (parent == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    char *last = backslash != NULL && (slash == NULL || backslash > slash) ? backslash : slash;
    if (last != NULL) {
        *last = 0;
    }
    spisor_status made = last == NULL ? SPISOR_OK : spisor_make_directories(parent);
    spisor_memory_release(parent);
    return made;
}

spisor_status runtime_write_holders(const char *path, const runtime_holders_record *record) {
    if (isnan(record->pid) && record->count == 0) {
        return runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE ? SPISOR_OK : SPISOR_REFUSED;
    }
    spisor_status made = make_parent(path);
    if (made != SPISOR_OK) {
        return made;
    }
    char *text = record_text(record);
    if (text == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    spisor_status written = spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    spisor_memory_release(text);
    return written;
}

int runtime_draft_started_host(const runtime_holders_record *record, runtime_holders_draft *draft) {
    runtime_holders_record started = *record;
    started.pid = 0;
    started.alive_at = 0;
    started.check_milliseconds = NAN;
    draft->tree = record_tree(&started);
    char *printed = draft->tree == NULL ? NULL : cJSON_PrintUnformatted(draft->tree);
    draft->capacity = printed == NULL ? 0 : strlen(printed) + 2 * NUMBER_TEXT_WIDTH + PREALLOCATED_SLACK + 2;
    cJSON_free(printed);
    draft->text = draft->capacity == 0 ? NULL : spisor_memory_allocate(draft->capacity);
    if (draft->text == NULL) {
        runtime_free_holders_draft(draft);
        return 0;
    }
    return 1;
}

int runtime_print_started_host(runtime_holders_draft *draft, double pid, double alive_at) {
    (void)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(draft->tree, "pid"), pid);
    (void)cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(draft->tree, "aliveAt"), alive_at);
    if (!cJSON_PrintPreallocated(draft->tree, draft->text, (int)draft->capacity - 1, 0)) {
        return 0;
    }
    size_t length = strlen(draft->text);
    draft->text[length] = '\n';
    draft->text[length + 1] = '\0';
    return 1;
}

spisor_status runtime_write_holders_draft(const char *path, const runtime_holders_draft *draft) {
    return spisor_replace_file(path, (const uint8_t *)draft->text, strlen(draft->text));
}

void runtime_free_holders_draft(runtime_holders_draft *draft) {
    cJSON_Delete(draft->tree);
    spisor_memory_release(draft->text);
    draft->tree = NULL;
    draft->text = NULL;
    draft->capacity = 0;
}

void runtime_keep_live_holders(runtime_holders_record *record, double dropped) {
    size_t kept = 0;
    for (size_t index = 0; index < record->count; index++) {
        double holder = record->holders[index];
        if (holder != dropped && runtime_process_is_alive(holder)) {
            record->holders[kept++] = holder;
        }
    }
    record->count = kept;
}

int runtime_add_holder(runtime_holders_record *record, double holder) {
    double *grown = spisor_memory_reallocate(record->holders, (record->count + 1) * sizeof *grown);
    if (grown == NULL) {
        return 0;
    }
    grown[record->count++] = holder;
    record->holders = grown;
    return 1;
}
