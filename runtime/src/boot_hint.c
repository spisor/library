#include "boot_hint.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_refusal.h"
#include "host_references.h"
#include "node_json_file.h"
#include "spisor/file.h"
#include "spisor/json_write.h"

static char *hosts_folder(const char *cache) {
    const char *segments[2] = {cache, "hosts"};
    return runtime_cache_joined(segments, 2);
}

char *runtime_boot_hint_path(const char *cache, const char *host_id, char **problem) {
    *problem = NULL;
    char *folder = hosts_folder(cache);
    char *path = folder == NULL ? NULL : runtime_host_file_path(folder, host_id, "libraries", problem);
    free(folder);
    return path;
}

static cJSON *string_entries(const cJSON *value) {
    cJSON *entries = cJSON_CreateObject();
    for (const cJSON *entry = cJSON_IsObject(value) ? value->child : NULL; entries != NULL && entry != NULL;
         entry = entry->next) {
        if (cJSON_IsString(entry) && cJSON_AddStringToObject(entries, entry->string, entry->valuestring) == NULL) {
            cJSON_Delete(entries);
            entries = NULL;
        }
    }
    return entries;
}

cJSON *runtime_boot_hint_of(const cJSON *recorded) {
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(recorded, "host");
    cJSON *hint = cJSON_IsObject(recorded) ? cJSON_CreateObject() : NULL;
    cJSON *libraries = hint == NULL ? NULL : string_entries(cJSON_GetObjectItemCaseSensitive(recorded, "libraries"));
    int attached = libraries != NULL && cJSON_AddItemToObject(hint, "libraries", libraries);
    if (!attached) {
        cJSON_Delete(libraries);
    }
    int built = attached &&
                (!cJSON_IsString(host) || cJSON_AddStringToObject(hint, "host", host->valuestring) != NULL);
    if (!built) {
        cJSON_Delete(hint);
        return NULL;
    }
    return hint;
}

cJSON *runtime_read_boot_hint(const char *cache, const char *host_id) {
    char *problem = NULL;
    char *path = runtime_boot_hint_path(cache, host_id, &problem);
    free(problem);
    cJSON *recorded = path == NULL ? NULL : runtime_read_node_json(path);
    free(path);
    cJSON *hint = runtime_boot_hint_of(recorded);
    cJSON_Delete(recorded);
    return hint;
}

/** The host version a write records: the one given, else the one the hint already names, else none. */
static char *recorded_host(const char *cache, const char *host_id, const char *host) {
    cJSON *hint = host == NULL ? runtime_read_boot_hint(cache, host_id) : NULL;
    const cJSON *kept = cJSON_GetObjectItemCaseSensitive(hint, "host");
    const char *version = host != NULL ? host : cJSON_IsString(kept) ? kept->valuestring : NULL;
    const char *parts[1] = {version};
    char *copy = version == NULL ? NULL : runtime_cache_text(parts, 1);
    cJSON_Delete(hint);
    return copy;
}

static char *hint_text(const cJSON *libraries, const char *host) {
    cJSON *hint = cJSON_CreateObject();
    int built = hint != NULL && cJSON_AddItemReferenceToObject(hint, "libraries", (cJSON *)libraries) &&
                (host == NULL || cJSON_AddStringToObject(hint, "host", host) != NULL);
    char *pretty = built ? spisor_json_stringify_pretty(hint) : NULL;
    cJSON_Delete(hint);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    free(pretty);
    return text;
}

static int write_text(const char *cache, const char *path, const char *text) {
    char *folder = hosts_folder(cache);
    int written = folder != NULL && spisor_make_directories(folder) &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    free(folder);
    return written;
}

int runtime_write_boot_hint(const char *cache, const char *host_id, const cJSON *libraries, const char *host,
                            char **problem) {
    char *path = runtime_boot_hint_path(cache, host_id, problem);
    char *version = path == NULL ? NULL : recorded_host(cache, host_id, host);
    char *text = path == NULL ? NULL : hint_text(libraries, version);
    int written = text != NULL && write_text(cache, path, text);
    if (!written && path != NULL) {
        const char *parts[3] = {"the boot hint ", path, " could not be written"};
        *problem = runtime_cache_text(parts, 3);
    }
    free(text);
    free(version);
    free(path);
    return written;
}

int runtime_hint_host_version(const char *cache, const char *host_id, const char *version) {
    cJSON *hint = runtime_read_boot_hint(cache, host_id);
    if (hint == NULL) {
        return 1;
    }
    char *problem = NULL;
    int written = runtime_write_boot_hint(cache, host_id, cJSON_GetObjectItemCaseSensitive(hint, "libraries"),
                                          version, &problem);
    free(problem);
    cJSON_Delete(hint);
    return written;
}
