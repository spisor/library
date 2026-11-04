#include "cache_references.h"

#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "host_references.h"
#include "node_json_file.h"
#include "run_records.h"


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

static cJSON *read_entry(const char *folder, const char *name) {
    const char *segments[2] = {folder, name};
    char *path = runtime_cache_joined(segments, 2);
    cJSON *value = path == NULL ? NULL : runtime_read_node_json(path);
    free(path);
    return value;
}

static int add_hint(cJSON *referenced, const char *hosts, const char *name) {
    size_t id_length = strlen(name) - strlen(RUNTIME_JSON_EXTENSION);
    cJSON *recorded = id_length == 0 ? NULL : read_entry(hosts, name);
    cJSON *hint = runtime_boot_hint_of(recorded);
    cJSON_Delete(recorded);
    int added = 1;
    if (hint != NULL) {
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(hint, "host");
        added = add_version_map(referenced, cJSON_GetObjectItemCaseSensitive(hint, "libraries"));
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
    cJSON *names = hosts == NULL ? NULL : runtime_cache_json_names(segments, 2);
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
    const cJSON *run = NULL;
    const cJSON *next = NULL;
    const cJSON *rollback = NULL;
    int added = 1;
    if (runtime_host_references_of(recorded, &run, &next, &rollback)) {
        added = add_version_map(referenced, run) && add_version_map(referenced, next) &&
                add_version_map(referenced, rollback);
    }
    cJSON_Delete(recorded);
    return added;
}

static int add_references(cJSON *referenced, const char *state) {
    const char *segments[2] = {state, "bundles"};
    char *folder = runtime_cache_joined(segments, 2);
    cJSON *names = folder == NULL ? NULL : runtime_cache_json_names(segments, 2);
    int added = names != NULL;
    for (const cJSON *name = added ? names->child : NULL; added && name != NULL; name = name->next) {
        added = add_host_references(referenced, folder, name->valuestring);
    }
    cJSON_Delete(names);
    free(folder);
    return added;
}

static int add_live_runs(cJSON *referenced, const char *state, double now) {
    cJSON *runs = runtime_live_runs(state, now);
    int added = runs != NULL;
    for (const cJSON *run = added ? runs->child : NULL; added && run != NULL; run = run->next) {
        added = add_version_map(referenced, run);
    }
    cJSON_Delete(runs);
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
