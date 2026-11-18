#include "host_references.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "directory_tree.h"
#include "json_service.h"
#include "node_json_file.h"
#include "run_records.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/json_write.h"

#define REFERENCE_EXTENSION ".json"

int runtime_host_references_of(const cJSON *recorded, const cJSON **run, const cJSON **next, const cJSON **rollback) {
    *run = cJSON_GetObjectItemCaseSensitive(recorded, "run");
    *next = cJSON_GetObjectItemCaseSensitive(recorded, "next");
    const cJSON *kept = cJSON_GetObjectItemCaseSensitive(recorded, "rollback");
    *rollback = runtime_is_version_map(kept) ? kept : NULL;
    return cJSON_IsObject(recorded) && runtime_is_version_map(*run) && runtime_is_version_map(*next);
}

int runtime_is_plain_host_id(const char *host_id) {
    return runtime_is_portable_segment(host_id);
}

static char *outside_problem(const char *folder, const char *host_id, const char *recording) {
    cJSON *name = cJSON_CreateString(host_id);
    char *quoted = name == NULL ? NULL : cJSON_PrintUnformatted(name);
    cJSON_Delete(name);
    const char *parts[6] = {"the host id ", quoted == NULL ? host_id : quoted, " would record its ", recording,
                            " outside ", folder};
    char *problem = runtime_cache_text(parts, 6);
    cJSON_free(quoted);
    return problem;
}

char *runtime_host_file_path(const char *folder, const char *host_id, const char *recording, char **problem) {
    *problem = NULL;
    if (!runtime_is_plain_host_id(host_id)) {
        *problem = outside_problem(folder, host_id, recording);
        return NULL;
    }
    const char *name_parts[2] = {host_id, REFERENCE_EXTENSION};
    char *name = runtime_cache_text(name_parts, 2);
    const char *segments[2] = {folder, name};
    char *path = name == NULL ? NULL : runtime_cache_joined(segments, 2);
    spisor_memory_release(name);
    return path;
}

static char *bundles_folder(const char *state) {
    const char *segments[2] = {state, "bundles"};
    return runtime_cache_joined(segments, 2);
}

char *runtime_host_references_path(const char *state, const char *host_id, char **problem) {
    *problem = NULL;
    char *folder = bundles_folder(state);
    char *path = folder == NULL ? NULL : runtime_host_file_path(folder, host_id, "bundles", problem);
    spisor_memory_release(folder);
    return path;
}

static cJSON *copy_or_empty(const cJSON *map) {
    return map == NULL ? cJSON_CreateObject() : cJSON_Duplicate(map, 1);
}

static int is_kept(const cJSON *map) {
    return runtime_is_version_map(map) && map->child != NULL;
}

static cJSON *copy_if_kept(const cJSON *map) {
    return is_kept(map) ? cJSON_Duplicate(map, 1) : NULL;
}

/** Owned copies of the members; every member NULL when memory runs out copying any of them. */
static runtime_host_references copied(const cJSON *run, const cJSON *next, const cJSON *rollback,
                                      const cJSON *rolled_back_from) {
    runtime_host_references references = {copy_or_empty(run), copy_or_empty(next), copy_if_kept(rollback),
                                          copy_if_kept(rolled_back_from)};
    int whole = references.run != NULL && references.next != NULL &&
                (references.rollback != NULL || !is_kept(rollback)) &&
                (references.rolled_back_from != NULL || !is_kept(rolled_back_from));
    if (!whole) {
        runtime_free_host_references(&references);
    }
    return references;
}

runtime_host_references runtime_read_host_references(const char *state, const char *host_id) {
    runtime_host_references none = {NULL, NULL, NULL, NULL};
    char *problem = NULL;
    char *path = runtime_host_references_path(state, host_id, &problem);
    spisor_memory_release(problem);
    if (path == NULL) {
        return none;
    }
    int out_of_memory;
    cJSON *recorded = runtime_read_node_json(path, &out_of_memory);
    spisor_memory_release(path);
    if (out_of_memory) {
        return none;
    }
    const cJSON *run = NULL;
    const cJSON *next = NULL;
    const cJSON *rollback = NULL;
    runtime_host_references references =
        runtime_host_references_of(recorded, &run, &next, &rollback)
            ? copied(run, next, rollback, cJSON_GetObjectItemCaseSensitive(recorded, "rolledBackFrom"))
            : copied(NULL, NULL, NULL, NULL);
    cJSON_Delete(recorded);
    if (references.run == NULL || references.next == NULL) {
        runtime_free_host_references(&references);
    }
    return references;
}

static char *references_text(const runtime_host_references *references) {
    cJSON *recorded = cJSON_CreateObject();
    int built = recorded != NULL && runtime_add_reference(recorded, "run", references->run) &&
                runtime_add_reference(recorded, "next", references->next) &&
                (references->rollback == NULL || references->rollback->child == NULL ||
                 runtime_add_reference(recorded, "rollback", references->rollback)) &&
                (references->rolled_back_from == NULL || references->rolled_back_from->child == NULL ||
                 runtime_add_reference(recorded, "rolledBackFrom", references->rolled_back_from));
    char *pretty = built ? spisor_json_stringify_pretty(recorded) : NULL;
    cJSON_Delete(recorded);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    spisor_memory_release(pretty);
    return text;
}

static spisor_status write_in_folder(const char *folder, const char *path, const char *text) {
    return spisor_make_directories(folder) ? spisor_replace_file(path, (const uint8_t *)text, strlen(text))
                                           : SPISOR_REFUSED;
}

/** Writes the references' text; 0 with *out_of_memory set to 1 when memory ran out. */
static int write_text(const char *state, const char *path, const char *text, int *out_of_memory) {
    char *folder = bundles_folder(state);
    spisor_status written = folder == NULL ? SPISOR_OUT_OF_MEMORY : write_in_folder(folder, path, text);
    *out_of_memory = written == SPISOR_OUT_OF_MEMORY;
    spisor_memory_release(folder);
    return written == SPISOR_OK;
}

int runtime_record_host_references(const char *state, const char *host_id, const runtime_host_references *references,
                                   char **problem) {
    char *path = runtime_host_references_path(state, host_id, problem);
    char *text = path == NULL ? NULL : references_text(references);
    int out_of_memory = 0;
    int written = text != NULL && write_text(state, path, text, &out_of_memory);
    if (!written && text != NULL && !out_of_memory) {
        const char *parts[3] = {"the references file ", path, " could not be written"};
        *problem = runtime_cache_text(parts, 3);
    }
    spisor_memory_release(text);
    spisor_memory_release(path);
    return written;
}

int runtime_draft_host_references(const char *state, const char *host_id, const runtime_host_references *references,
                                  runtime_references_draft *draft, char **problem) {
    draft->path = runtime_host_references_path(state, host_id, problem);
    draft->text = draft->path == NULL ? NULL : references_text(references);
    if (draft->text == NULL) {
        runtime_free_references_draft(draft);
        return 0;
    }
    return 1;
}

int runtime_write_references_draft(const runtime_references_draft *draft) {
    return spisor_replace_file(draft->path, (const uint8_t *)draft->text, strlen(draft->text)) == SPISOR_OK;
}

void runtime_free_references_draft(runtime_references_draft *draft) {
    spisor_memory_release(draft->path);
    spisor_memory_release(draft->text);
    draft->path = NULL;
    draft->text = NULL;
}

int runtime_remove_host_references(const char *state, const char *host_id) {
    char *problem = NULL;
    char *path = runtime_host_references_path(state, host_id, &problem);
    spisor_memory_release(problem);
    runtime_entry_outcome removed = path == NULL ? RUNTIME_ENTRY_FAILED : runtime_remove_file_entry(path);
    spisor_memory_release(path);
    return removed == RUNTIME_ENTRY_DONE && runtime_remove_run_records(state, host_id);
}

void runtime_free_host_references(runtime_host_references *references) {
    cJSON_Delete(references->run);
    cJSON_Delete(references->next);
    cJSON_Delete(references->rollback);
    cJSON_Delete(references->rolled_back_from);
    references->rolled_back_from = NULL;
    references->run = NULL;
    references->next = NULL;
    references->rollback = NULL;
}
