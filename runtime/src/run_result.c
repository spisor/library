#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "run_result.h"
#include "allocation.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#include "atomic.h"
#include "cJSON.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "directory_tree.h"
#include "paths.h"
#include "segment.h"
#include "spisor/definition.h"
#include "spisor/file.h"
#include "spisor/json_value.h"
#include "spisor/path.h"

#define NONE_JSON "null"

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

static int is_dot_segment(const char *start, size_t length) {
    return (length == 1 && start[0] == '.') || (length == 2 && start[0] == '.' && start[1] == '.');
}

/** The offset of path's last separator, or none when no segment of path is `.` or `..`; -1 for either failing. */
static long last_separator_of_plain(const char *path) {
    long last = -1;
    size_t start = 0;
    for (size_t index = 0;; index++) {
        if (path[index] == '\0' || is_separator(path[index])) {
            if (is_dot_segment(path + start, index - start)) {
                return -1;
            }
            if (path[index] == '\0') {
                return last;
            }
            last = (long)index;
            start = index + 1;
        }
    }
}

static char *results_folder(const char *state) {
    const char *segments[1] = {RUNTIME_RESULTS_FOLDER};
    return runtime_cache_resolve(state, segments, 1);
}

/** path's folder as written: everything before its last separator, the separator kept when it is the root's. */
static char *folder_of(const char *path, long separator) {
    int root = separator == 0 || (RUNTIME_HOST_IS_WINDOWS && separator == 2 && path[1] == ':');
    size_t length = root ? (size_t)separator + 1 : (size_t)separator;
    char *folder = spisor_memory_allocate(length + 1);
    if (folder != NULL) {
        memcpy(folder, path, length);
        folder[length] = '\0';
    }
    return folder;
}

int runtime_is_result_path(const char *state, const char *path, int *out_of_memory) {
    long separator = path == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, path)
                         ? -1
                         : last_separator_of_plain(path);
    if (separator < 0 || !runtime_is_portable_segment(path + separator + 1)) {
        return 0;
    }
    char *results = results_folder(state);
    char *folder = folder_of(path, separator);
    char *resolved_folder = folder == NULL ? NULL : runtime_cache_resolve(folder, NULL, 0);
    char *resolved = runtime_cache_resolve(path, NULL, 0);
    int inside = results != NULL && resolved_folder != NULL && resolved != NULL &&
                 runtime_cache_is_strictly_inside(results, resolved) &&
                 !runtime_cache_is_strictly_inside(results, resolved_folder);
    *out_of_memory = results == NULL || folder == NULL || resolved_folder == NULL || resolved == NULL;
    spisor_memory_release(resolved);
    spisor_memory_release(resolved_folder);
    spisor_memory_release(folder);
    spisor_memory_release(results);
    return inside;
}

static spisor_status refuse_write(spisor_refusal *refusal, const char *lead, const char *path, const char *tail,
                                  const char *fix) {
    const char *parts[3] = {lead, path, tail};
    char *detail = runtime_cache_text(parts, 3);
    spisor_status status = detail == NULL ? SPISOR_OUT_OF_MEMORY
                                          : spisor_refuse_as(refusal, SPISOR_REFUSED, detail, fix);
    spisor_memory_release(detail);
    return status;
}

/** The results folder made when missing: OK when a real folder stands there, REFUSED as the write says else. */
static spisor_status ready_folder(const char *state, spisor_refusal *refusal) {
    char *results = results_folder(state);
    spisor_status status = results == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(results);
    runtime_entry_kind kind = status == SPISOR_OUT_OF_MEMORY ? RUNTIME_ENTRY_PROBE_OUT_OF_MEMORY
                                                             : runtime_probe_entry(results);
    if (kind == RUNTIME_ENTRY_PROBE_OUT_OF_MEMORY) {
        status = SPISOR_OUT_OF_MEMORY;
    } else if (kind != RUNTIME_ENTRY_DIRECTORY) {
        status = refuse_write(refusal, "the results folder ", results,
                              " is not a folder of its own, so no result is written into it",
                              "remove what stands at the results folder, then run the action again");
    } else {
        status = SPISOR_OK;
    }
    spisor_memory_release(results);
    return status;
}

/** result as the compact JSON the file holds, freed with cJSON_free, through *text. */
static spisor_status result_json(const spisor_value *result, char **text) {
    cJSON *json = NULL;
    spisor_status status;
    *text = NULL;
    if (result->kind == SPISOR_VALUE_NONE) {
        *text = cJSON_malloc(sizeof NONE_JSON);
        if (*text != NULL) {
            memcpy(*text, NONE_JSON, sizeof NONE_JSON);
        }
        return *text == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    }
    status = spisor_json_of_value(result, &json);
    if (status == SPISOR_OK) {
        *text = cJSON_PrintUnformatted(json);
        status = *text == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    }
    cJSON_Delete(json);
    return status;
}

/**
 * @implNote The folder is probed before the file is created in it, so a writer of the state place who swaps the folder
 * for a link in between leads the file through it; accepted, since that writer can already change what reads the
 * state place.
 */
static spisor_status create_file(const char *path, const char *text, spisor_refusal *refusal) {
    runtime_entry_outcome outcome = runtime_create_new_file(path, (const uint8_t *)text, strlen(text), 0);
    if (outcome == RUNTIME_ENTRY_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (outcome == RUNTIME_ENTRY_EXISTS) {
        return refuse_write(refusal, "something already stands at ", path,
                            ", so the action's result is not written over it",
                            "name a result path nothing stands at, then run the action again");
    }
    if (outcome != RUNTIME_ENTRY_DONE) {
        return refuse_write(refusal, "the action's result could not be written to ", path, "",
                            "check that the state place can be written, then run the action again");
    }
    return SPISOR_OK;
}

spisor_status runtime_write_result(const char *state, const char *path, const spisor_value *result,
                                   spisor_refusal *refusal) {
    char *text = NULL;
    spisor_status status = result_json(result, &text);
    if (status == SPISOR_MALFORMED) {
        status = spisor_refuse_as(refusal, SPISOR_MALFORMED, "the action's result is no well-formed value tree",
                                  "answer the action with a value as weivel/value.h defines one");
    }
    if (status == SPISOR_OK) {
        status = ready_folder(state, refusal);
    }
    if (status == SPISOR_OK) {
        status = create_file(path, text, refusal);
    }
    cJSON_free(text);
    return status;
}

char *runtime_fresh_result_path(const char *state, int *removable) {
    static volatile long taken;
    char name[64];
    snprintf(name, sizeof name, "%.0f-%ld.json", (double)current_process_id(), runtime_atomic_increment(&taken));
    const char *segments[3] = {state, RUNTIME_RESULTS_FOLDER, name};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
    runtime_entry_outcome cleared = path == NULL ? RUNTIME_ENTRY_OUT_OF_MEMORY : runtime_remove_file_entry(path);
    *removable = cleared == RUNTIME_ENTRY_DONE || cleared == RUNTIME_ENTRY_OUT_OF_MEMORY;
    if (cleared != RUNTIME_ENTRY_DONE) {
        spisor_memory_release(path);
        return NULL;
    }
    return path;
}

char *runtime_take_result(const char *path, int exit_code, int *out_of_memory) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    char *text = exit_code == 0 ? (char *)spisor_read_file_reporting(path, &length, &outcome) : NULL;
    runtime_entry_outcome removed = runtime_remove_file_entry(path);
    *out_of_memory = removed == RUNTIME_ENTRY_OUT_OF_MEMORY || outcome == SPISOR_READ_OUT_OF_MEMORY;
    if (removed == RUNTIME_ENTRY_OUT_OF_MEMORY) {
        spisor_memory_release(text);
        text = NULL;
        runtime_remove_file_entry(path);
    }
    return text;
}
