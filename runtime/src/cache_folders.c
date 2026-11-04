#include "cache_folders.h"

#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "paths.h"
#include "spisor/path.h"

char *runtime_cache_joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

cJSON *runtime_cache_folder_names(const char *const *segments, size_t count) {
    char *path = runtime_cache_joined(segments, count);
    cJSON *names = path == NULL ? NULL : cJSON_CreateArray();
    runtime_directory_listing listing;
    if (names != NULL && runtime_list_directory(path, &listing)) {
        for (size_t index = 0; names != NULL && index < listing.count; index++) {
            cJSON *name = listing.entries[index].is_directory ? cJSON_CreateString(listing.entries[index].name) : NULL;
            if (listing.entries[index].is_directory && (name == NULL || !cJSON_AddItemToArray(names, name))) {
                cJSON_Delete(name);
                cJSON_Delete(names);
                names = NULL;
            }
        }
        runtime_free_directory_listing(&listing);
    }
    free(path);
    return names;
}

static int is_json_name(const char *name) {
    size_t length = strlen(name);
    return length >= strlen(RUNTIME_JSON_EXTENSION) &&
           strcmp(name + length - strlen(RUNTIME_JSON_EXTENSION), RUNTIME_JSON_EXTENSION) == 0;
}

cJSON *runtime_cache_json_names(const char *const *segments, size_t count) {
    char *folder = runtime_cache_joined(segments, count);
    cJSON *names = folder == NULL ? NULL : cJSON_CreateArray();
    runtime_directory_listing listing;
    if (names != NULL && runtime_list_directory(folder, &listing)) {
        for (size_t index = 0; names != NULL && index < listing.count; index++) {
            const char *name = listing.entries[index].name;
            cJSON *entry = is_json_name(name) ? cJSON_CreateString(name) : NULL;
            if (is_json_name(name) && (entry == NULL || !cJSON_AddItemToArray(names, entry))) {
                cJSON_Delete(entry);
                cJSON_Delete(names);
                names = NULL;
            }
        }
        runtime_free_directory_listing(&listing);
    }
    free(folder);
    return names;
}
