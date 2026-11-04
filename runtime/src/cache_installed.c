#include "cache_installed.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_marker.h"

static int is_marked(const char *cache, const char *root, const char *id, const char *version, const char *platform) {
    const char *segments[5] = {cache, root, id, version, platform};
    char *directory = runtime_cache_joined(segments, platform == NULL ? 4 : 5);
    int marked = directory != NULL && runtime_cache_has_marker(directory);
    free(directory);
    return marked;
}

/** The versions of id marked complete, or NULL when memory runs out. */
static cJSON *marked_versions(const char *cache, const char *root, const char *id, const char *platform) {
    const char *segments[3] = {cache, root, id};
    cJSON *versions = runtime_cache_folder_names(segments, 3);
    cJSON *version = versions == NULL ? NULL : versions->child;
    while (version != NULL) {
        cJSON *next = version->next;
        if (!is_marked(cache, root, id, version->valuestring, platform)) {
            cJSON_Delete(cJSON_DetachItemViaPointer(versions, version));
        }
        version = next;
    }
    return versions;
}

/** Adds id's versions to installed unless there are none; 0 when memory runs out. */
static int add_versions(cJSON *installed, const char *id, cJSON *versions) {
    if (versions == NULL) {
        return 0;
    }
    if (cJSON_GetArraySize(versions) == 0) {
        cJSON_Delete(versions);
        return 1;
    }
    if (!cJSON_AddItemToObject(installed, id, versions)) {
        cJSON_Delete(versions);
        return 0;
    }
    return 1;
}

static cJSON *installed_under(const char *cache, const char *root, const char *platform) {
    const char *segments[2] = {cache, root};
    cJSON *ids = runtime_cache_folder_names(segments, 2);
    cJSON *installed = ids == NULL ? NULL : cJSON_CreateObject();
    for (const cJSON *id = installed == NULL ? NULL : ids->child; id != NULL; id = id->next) {
        if (!add_versions(installed, id->valuestring, marked_versions(cache, root, id->valuestring, platform))) {
            cJSON_Delete(installed);
            installed = NULL;
            break;
        }
    }
    cJSON_Delete(ids);
    return installed;
}

cJSON *runtime_cache_installed_bundles(const char *cache) {
    return installed_under(cache, "bundles", NULL);
}

cJSON *runtime_cache_installed_libraries(const char *cache, const char *platform) {
    return installed_under(cache, "libraries", platform);
}

static int compare_names(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/** Every platform folder name under every library version, duplicates included; NULL when memory runs out. */
static cJSON *platform_folders(const char *cache) {
    const char *root[2] = {cache, "libraries"};
    cJSON *ids = runtime_cache_folder_names(root, 2);
    cJSON *found = ids == NULL ? NULL : cJSON_CreateArray();
    for (const cJSON *id = found == NULL ? NULL : ids->child; found != NULL && id != NULL; id = id->next) {
        const char *id_segments[3] = {cache, "libraries", id->valuestring};
        cJSON *versions = runtime_cache_folder_names(id_segments, 3);
        for (const cJSON *version = versions == NULL ? NULL : versions->child; version != NULL; version = version->next) {
            const char *version_segments[4] = {cache, "libraries", id->valuestring, version->valuestring};
            cJSON *platforms = runtime_cache_folder_names(version_segments, 4);
            while (platforms != NULL && platforms->child != NULL) {
                cJSON_AddItemToArray(found, cJSON_DetachItemViaPointer(platforms, platforms->child));
            }
            cJSON_Delete(platforms);
        }
        cJSON_Delete(versions);
    }
    cJSON_Delete(ids);
    return found;
}

cJSON *runtime_cache_library_platforms(const char *cache) {
    cJSON *found = platform_folders(cache);
    int count = found == NULL ? 0 : cJSON_GetArraySize(found);
    const char **names = count == 0 ? NULL : malloc((size_t)count * sizeof *names);
    cJSON *platforms = found == NULL || (count > 0 && names == NULL) ? NULL : cJSON_CreateArray();
    int index = 0;
    for (const cJSON *name = platforms == NULL ? NULL : found->child; name != NULL; name = name->next) {
        names[index++] = name->valuestring;
    }
    if (platforms != NULL && count > 1) {
        qsort(names, (size_t)count, sizeof *names, compare_names);
    }
    for (index = 0; platforms != NULL && index < count; index++) {
        if (index == 0 || strcmp(names[index], names[index - 1]) != 0) {
            cJSON_AddItemToArray(platforms, cJSON_CreateString(names[index]));
        }
    }
    free((void *)names);
    cJSON_Delete(found);
    return platforms;
}
