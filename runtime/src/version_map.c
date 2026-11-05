#include "version_map.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_marker.h"
#include "cache_place.h"

const char *runtime_map_version(const cJSON *map, const char *id) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(map, id);
    return cJSON_IsString(version) ? version->valuestring : NULL;
}

int runtime_map_set(cJSON *map, const char *id, const char *version) {
    cJSON *value = cJSON_CreateString(version);
    if (value == NULL) {
        return 0;
    }
    if (cJSON_GetObjectItemCaseSensitive(map, id) != NULL) {
        return cJSON_ReplaceItemInObjectCaseSensitive(map, id, value);
    }
    if (!cJSON_AddItemToObject(map, id, value)) {
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

/** Whether check, which it frees, answers intact. */
static int is_intact(cJSON *check) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(check, "kind");
    int intact = cJSON_IsString(kind) && strcmp(kind->valuestring, "intact") == 0;
    cJSON_Delete(check);
    return intact;
}

/** cache/bundles/<id>/<version>, freed with free; NULL when either would climb out of its folder. */
static char *bundle_directory(const char *cache, const char *id, const char *version) {
    const char *segments[2] = {cache, "bundles"};
    char *bundles = runtime_cache_joined(segments, 2);
    char *versions = bundles == NULL ? NULL : runtime_cache_resolve(bundles, &id, 1);
    char *directory = versions == NULL ? NULL : runtime_cache_resolve(versions, &version, 1);
    int inside = directory != NULL && id[0] != 0 && version[0] != 0 &&
                 runtime_cache_is_strictly_inside(bundles, versions) &&
                 runtime_cache_is_strictly_inside(versions, directory);
    if (!inside) {
        free(directory);
        directory = NULL;
    }
    free(versions);
    free(bundles);
    return directory;
}

int runtime_bundle_is_sized(const char *cache, const char *id, const char *version) {
    char *directory = bundle_directory(cache, id, version);
    int sized = directory != NULL && is_intact(runtime_cache_check_version_sizes(directory));
    free(directory);
    return sized;
}

int runtime_bundle_is_intact(const char *cache, const char *id, const char *version) {
    char *directory = bundle_directory(cache, id, version);
    int intact = directory != NULL && is_intact(runtime_cache_check_version_sizes(directory)) &&
                 is_intact(runtime_cache_check_version(directory));
    free(directory);
    return intact;
}
