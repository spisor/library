#include "host_references.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "directory_tree.h"
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
    char *segment = runtime_portable_segment(host_id);
    int plain = segment != NULL && strcmp(segment, host_id) == 0;
    free(segment);
    return plain;
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
    free(name);
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
    free(folder);
    return path;
}

static cJSON *copy_or_empty(const cJSON *map) {
    return map == NULL ? cJSON_CreateObject() : cJSON_Duplicate(map, 1);
}

static runtime_host_references copied(const cJSON *run, const cJSON *next, const cJSON *rollback) {
    runtime_host_references references = {copy_or_empty(run), copy_or_empty(next), NULL};
    if (rollback != NULL && rollback->child != NULL) {
        references.rollback = cJSON_Duplicate(rollback, 1);
    }
    return references;
}

runtime_host_references runtime_read_host_references(const char *state, const char *host_id) {
    runtime_host_references none = {NULL, NULL, NULL};
    char *problem = NULL;
    char *path = runtime_host_references_path(state, host_id, &problem);
    free(problem);
    if (path == NULL) {
        return none;
    }
    cJSON *recorded = runtime_read_node_json(path);
    free(path);
    const cJSON *run = NULL;
    const cJSON *next = NULL;
    const cJSON *rollback = NULL;
    runtime_host_references references = runtime_host_references_of(recorded, &run, &next, &rollback)
                                             ? copied(run, next, rollback)
                                             : copied(NULL, NULL, NULL);
    cJSON_Delete(recorded);
    if (references.run == NULL || references.next == NULL) {
        runtime_free_host_references(&references);
    }
    return references;
}

static char *references_text(const runtime_host_references *references) {
    cJSON *recorded = cJSON_CreateObject();
    int built = recorded != NULL && cJSON_AddItemReferenceToObject(recorded, "run", references->run) &&
                cJSON_AddItemReferenceToObject(recorded, "next", references->next) &&
                (references->rollback == NULL || references->rollback->child == NULL ||
                 cJSON_AddItemReferenceToObject(recorded, "rollback", references->rollback));
    char *pretty = built ? spisor_json_stringify_pretty(recorded) : NULL;
    cJSON_Delete(recorded);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    free(pretty);
    return text;
}

static int write_text(const char *state, const char *path, const char *text) {
    char *folder = bundles_folder(state);
    int written = folder != NULL && spisor_make_directories(folder) &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    free(folder);
    return written;
}

int runtime_record_host_references(const char *state, const char *host_id, const runtime_host_references *references,
                                   char **problem) {
    char *path = runtime_host_references_path(state, host_id, problem);
    char *text = path == NULL ? NULL : references_text(references);
    int written = text != NULL && write_text(state, path, text);
    if (!written && path != NULL) {
        const char *parts[3] = {"the references file ", path, " could not be written"};
        *problem = runtime_cache_text(parts, 3);
    }
    free(text);
    free(path);
    return written;
}

int runtime_remove_host_references(const char *state, const char *host_id) {
    char *problem = NULL;
    char *path = runtime_host_references_path(state, host_id, &problem);
    free(problem);
    runtime_entry_outcome removed = path == NULL ? RUNTIME_ENTRY_FAILED : runtime_remove_file_entry(path);
    free(path);
    return removed == RUNTIME_ENTRY_DONE && runtime_remove_run_records(state, host_id);
}

void runtime_free_host_references(runtime_host_references *references) {
    cJSON_Delete(references->run);
    cJSON_Delete(references->next);
    cJSON_Delete(references->rollback);
    references->run = NULL;
    references->next = NULL;
    references->rollback = NULL;
}
