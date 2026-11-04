#include "cache_references.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "directory_tree.h"
#include "node_json_file.h"
#include "process_alive.h"
#include "spisor/file.h"
#include "spisor/javascript.h"
#include "spisor/json_kind.h"

#define JSON_EXTENSION ".json"
#define MISSED_CHECKS_BEFORE_DEAD 3.0
#define DEFAULT_CHECK_MILLISECONDS 5000.0

static int add_reference(cJSON *referenced, const char *id, const char *version) {
    const char *parts[3] = {id, "@", version};
    char *name = runtime_cache_text(parts, 3);
    int added = name != NULL && (cJSON_GetObjectItemCaseSensitive(referenced, name) != NULL ||
                                 cJSON_AddTrueToObject(referenced, name) != NULL);
    free(name);
    return added;
}

static int add_version_map(cJSON *referenced, const cJSON *versions) {
    int added = 1;
    for (const cJSON *member = versions == NULL ? NULL : versions->child; added && member != NULL; member = member->next) {
        added = !cJSON_IsString(member) || add_reference(referenced, member->string, member->valuestring);
    }
    return added;
}

/** The base name of each .json entry directly inside the folder, in name order; NULL when memory runs out. */
static cJSON *json_names(const char *const *segments, size_t count) {
    char *folder = runtime_cache_joined(segments, count);
    cJSON *names = folder == NULL ? NULL : cJSON_CreateArray();
    runtime_directory_listing listing;
    if (names != NULL && runtime_list_directory(folder, &listing)) {
        for (size_t index = 0; index < listing.count; index++) {
            const char *name = listing.entries[index].name;
            size_t length = strlen(name);
            if (length >= strlen(JSON_EXTENSION) && strcmp(name + length - strlen(JSON_EXTENSION), JSON_EXTENSION) == 0) {
                cJSON_AddItemToArray(names, cJSON_CreateString(name));
            }
        }
        runtime_free_directory_listing(&listing);
    }
    free(folder);
    return names;
}

static cJSON *read_entry(const char *folder, const char *name) {
    const char *segments[2] = {folder, name};
    char *path = runtime_cache_joined(segments, 2);
    cJSON *value = path == NULL ? NULL : runtime_read_node_json(path);
    free(path);
    return value;
}

static int add_hint(cJSON *referenced, const char *hosts, const char *name) {
    size_t id_length = strlen(name) - strlen(JSON_EXTENSION);
    cJSON *hint = id_length == 0 ? NULL : read_entry(hosts, name);
    int added = 1;
    if (cJSON_IsObject(hint)) {
        const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(hint, "libraries");
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(hint, "host");
        added = add_version_map(referenced, cJSON_IsObject(libraries) ? libraries : NULL);
        if (added && cJSON_IsString(host)) {
            char *id = malloc(id_length + 1);
            added = id != NULL;
            if (added) {
                memcpy(id, name, id_length);
                id[id_length] = 0;
                added = add_reference(referenced, id, host->valuestring);
            }
            free(id);
        }
    }
    cJSON_Delete(hint);
    return added;
}

static int add_hints(cJSON *referenced, const char *cache) {
    const char *segments[2] = {cache, "hosts"};
    char *hosts = runtime_cache_joined(segments, 2);
    cJSON *names = hosts == NULL ? NULL : json_names(segments, 2);
    int added = names != NULL;
    for (const cJSON *name = added ? names->child : NULL; added && name != NULL; name = name->next) {
        added = add_hint(referenced, hosts, name->valuestring);
    }
    cJSON_Delete(names);
    free(hosts);
    return added;
}

static int add_host_references(cJSON *referenced, const char *folder, const char *name) {
    cJSON *recorded = read_entry(folder, name);
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(recorded, "run");
    const cJSON *next = cJSON_GetObjectItemCaseSensitive(recorded, "next");
    const cJSON *rollback = cJSON_GetObjectItemCaseSensitive(recorded, "rollback");
    int added = 1;
    if (cJSON_IsObject(recorded) && runtime_is_version_map(run) && runtime_is_version_map(next)) {
        added = add_version_map(referenced, run) && add_version_map(referenced, next) &&
                (!runtime_is_version_map(rollback) || add_version_map(referenced, rollback));
    }
    cJSON_Delete(recorded);
    return added;
}

static int add_references(cJSON *referenced, const char *state) {
    const char *segments[2] = {state, "bundles"};
    char *folder = runtime_cache_joined(segments, 2);
    cJSON *names = folder == NULL ? NULL : json_names(segments, 2);
    int added = names != NULL;
    for (const cJSON *name = added ? names->child : NULL; added && name != NULL; name = name->next) {
        added = add_host_references(referenced, folder, name->valuestring);
    }
    cJSON_Delete(names);
    free(folder);
    return added;
}

static double number_or_nan(const cJSON *record, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(record, member);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) ? value->valuedouble : NAN;
}

/** @implNote processes/holders.ts isStampFresh: alive, and stamped within three of its own intervals. */
static int is_stamp_fresh(const cJSON *record, double now) {
    double pid = number_or_nan(record, "pid");
    double alive_at = number_or_nan(record, "aliveAt");
    double check = number_or_nan(record, "checkMilliseconds");
    if (isnan(pid) || isnan(alive_at) || !runtime_process_is_alive(pid)) {
        return 0;
    }
    return now - alive_at <= MISSED_CHECKS_BEFORE_DEAD * (isnan(check) ? DEFAULT_CHECK_MILLISECONDS : check);
}

static cJSON *parsed_record(const uint8_t *bytes, size_t length) {
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, length, &decoded_length);
    cJSON *record = decoded == NULL ? NULL : spisor_parse_json(decoded, decoded_length);
    free(decoded);
    return record;
}

/** Adds what a live run record names; deletes one whose start is gone or whose text is no record. */
static int add_run_record(cJSON *referenced, const char *path, double now) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    if (bytes == NULL) {
        return 1;
    }
    cJSON *record = parsed_record(bytes, length);
    free(bytes);
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(record, "run");
    int live = cJSON_IsObject(record) && runtime_is_version_map(run) && is_stamp_fresh(record, now);
    int added = 1;
    if (live) {
        added = add_version_map(referenced, run);
    } else {
        runtime_remove_file_entry(path);
    }
    cJSON_Delete(record);
    return added;
}

static int add_host_runs(cJSON *referenced, const char *state, const char *host, double now) {
    const char *segments[4] = {state, "bundles", "runs", host};
    cJSON *names = json_names(segments, 4);
    int added = names != NULL;
    for (const cJSON *name = added ? names->child : NULL; added && name != NULL; name = name->next) {
        const char *file_segments[5] = {state, "bundles", "runs", host, name->valuestring};
        char *path = runtime_cache_joined(file_segments, 5);
        added = path != NULL && add_run_record(referenced, path, now);
        free(path);
    }
    cJSON_Delete(names);
    return added;
}

static int add_live_runs(cJSON *referenced, const char *state, double now) {
    const char *segments[3] = {state, "bundles", "runs"};
    cJSON *hosts = runtime_cache_folder_names(segments, 3);
    int added = hosts != NULL;
    for (const cJSON *host = added ? hosts->child : NULL; added && host != NULL; host = host->next) {
        added = add_host_runs(referenced, state, host->valuestring, now);
    }
    cJSON_Delete(hosts);
    return added;
}

cJSON *runtime_cache_referenced(const char *cache, const char *state, double now) {
    cJSON *referenced = cJSON_CreateObject();
    if (referenced == NULL || !add_hints(referenced, cache) || !add_references(referenced, state) ||
        !add_live_runs(referenced, state, now)) {
        cJSON_Delete(referenced);
        return NULL;
    }
    return referenced;
}
