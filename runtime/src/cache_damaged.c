#include "cache_damaged.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_installed.h"
#include "cache_marker.h"
#include "identity.h"

static int is_kind(const cJSON *check, const char *expected) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(check, "kind");
    return cJSON_IsString(kind) && strcmp(kind->valuestring, expected) == 0;
}

static int is_intact(const cJSON *check) {
    return is_kind(check, "intact");
}

/** The first of the size check and the full hash check that finds the version in directory damaged, or an intact answer. */
static cJSON *verdict_of(const char *directory) {
    cJSON *sizes = runtime_cache_check_version_sizes(directory);
    if (sizes == NULL || !is_intact(sizes)) {
        return sizes;
    }
    cJSON_Delete(sizes);
    return runtime_cache_check_version(directory);
}

cJSON *runtime_cache_version_damage(const char *directory, int *out_of_memory) {
    cJSON *verdict = verdict_of(directory);
    *out_of_memory = verdict == NULL;
    if (verdict == NULL || !is_kind(verdict, "damaged")) {
        cJSON_Delete(verdict);
        return NULL;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(verdict, "kind");
    return verdict;
}

/** Adds {id, version, detail, fix, file?} to damaged when the version is damaged; 0 when memory runs out. */
static int add_verdict(cJSON *damaged, const char *id, const char *version, const char *directory) {
    int out_of_memory = directory == NULL;
    cJSON *verdict = directory == NULL ? NULL : runtime_cache_version_damage(directory, &out_of_memory);
    if (verdict == NULL) {
        return !out_of_memory;
    }
    int added = cJSON_AddStringToObject(verdict, "version", version) != NULL && cJSON_AddStringToObject(verdict, "id", id) != NULL &&
                cJSON_AddItemToArray(damaged, verdict);
    if (!added) {
        cJSON_Delete(verdict);
    }
    return added;
}

static int add_damaged_under(cJSON *damaged, const char *cache, const char *root, const cJSON *installed, const char *platform) {
    const cJSON *versions;
    cJSON_ArrayForEach(versions, installed) {
        const cJSON *version;
        cJSON_ArrayForEach(version, versions) {
            int unfit = 0;
            char *folder = runtime_item_folder(versions->string, &unfit);
            const char *segments[5] = {cache, root, folder, version->valuestring, platform};
            char *directory = folder == NULL ? NULL : runtime_cache_joined(segments, platform == NULL ? 4 : 5);
            int added = add_verdict(damaged, versions->string, version->valuestring, directory);
            free(directory);
            free(folder);
            if (!added) {
                return 0;
            }
        }
    }
    return 1;
}

cJSON *runtime_cache_damaged_versions(const char *cache, const char *platform) {
    cJSON *bundles = runtime_cache_installed_bundles(cache);
    cJSON *libraries = runtime_cache_installed_libraries(cache, platform);
    cJSON *damaged = bundles == NULL || libraries == NULL ? NULL : cJSON_CreateArray();
    if (damaged != NULL && (!add_damaged_under(damaged, cache, "bundles", bundles, NULL) ||
                            !add_damaged_under(damaged, cache, "libraries", libraries, platform))) {
        cJSON_Delete(damaged);
        damaged = NULL;
    }
    cJSON_Delete(libraries);
    cJSON_Delete(bundles);
    return damaged;
}
