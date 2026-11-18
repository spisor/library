#include "version_map.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "segment.h"

const char *runtime_map_version(const cJSON *map, const char *id) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(map, id);
    return cJSON_IsString(version) ? version->valuestring : NULL;
}

int runtime_map_set(cJSON *map, const char *id, const char *version) {
    cJSON *value = cJSON_CreateString(version);
    if (value == NULL) {
        return 0;
    }
    int placed = cJSON_GetObjectItemCaseSensitive(map, id) != NULL
                     ? cJSON_ReplaceItemInObjectCaseSensitive(map, id, value)
                     : cJSON_AddItemToObject(map, id, value);
    if (!placed) {
        cJSON_Delete(value);
        return 0;
    }
    return 1;
}

int runtime_map_merge(cJSON *into, const cJSON *from) {
    int merged = 1;
    for (const cJSON *entry = from == NULL ? NULL : from->child; merged && entry != NULL; entry = entry->next) {
        merged = !cJSON_IsString(entry) || runtime_map_set(into, entry->string, entry->valuestring);
    }
    return merged;
}

/** Whether check, which it frees, answers intact; a NULL check is memory running out. */
static int is_intact(cJSON *check, int *out_of_memory) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(check, "kind");
    int intact = cJSON_IsString(kind) && strcmp(kind->valuestring, "intact") == 0;
    *out_of_memory = *out_of_memory || check == NULL;
    cJSON_Delete(check);
    return intact;
}

/** The version's folder, or NULL with *out_of_memory set unless the id or version would climb out of it. */
static char *bundle_directory(const char *cache, const char *id, const char *version, int *out_of_memory) {
    char *directory = runtime_cache_bundle_directory(cache, id, version);
    *out_of_memory = directory == NULL && runtime_is_portable_segment(id) && runtime_is_portable_segment(version);
    return directory;
}

int runtime_bundle_is_sized(const char *cache, const char *id, const char *version, int *out_of_memory) {
    char *directory = bundle_directory(cache, id, version, out_of_memory);
    int sized = directory != NULL && is_intact(runtime_cache_check_version_sizes(directory), out_of_memory);
    spisor_memory_release(directory);
    return sized;
}

int runtime_bundle_is_intact(const char *cache, const char *id, const char *version, int *out_of_memory) {
    char *directory = bundle_directory(cache, id, version, out_of_memory);
    int intact = directory != NULL && is_intact(runtime_cache_check_version_sizes(directory), out_of_memory) &&
                 is_intact(runtime_cache_check_version(directory), out_of_memory);
    spisor_memory_release(directory);
    return intact;
}
