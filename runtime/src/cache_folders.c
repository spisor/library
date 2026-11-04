#include "cache_folders.h"

#include <stdlib.h>

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
