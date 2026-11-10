#include "cache_place.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "catalog_path.h"
#include "identity.h"
#include "paths.h"
#include "spisor/path.h"

static char *copy_of(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static char *followed_by(char *path, const char *segment) {
    if (spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, segment)) {
        free(path);
        return copy_of(segment);
    }
    const char *parts[2] = {path, segment};
    char *joined = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, parts, 2);
    free(path);
    return joined;
}

char *runtime_cache_resolve(const char *base, const char *const *segments, size_t count) {
    char *path = copy_of(base);
    for (size_t index = 0; path != NULL && index < count; index++) {
        path = followed_by(path, segments[index]);
    }
    char *resolved = path == NULL ? NULL : runtime_catalog_resolve_path(path);
    free(path);
    return resolved;
}

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

static int same_characters(const char *left, const char *right, size_t length) {
    if (!RUNTIME_HOST_IS_WINDOWS) {
        return memcmp(left, right, length) == 0;
    }
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) {
            return 0;
        }
    }
    return 1;
}

int runtime_cache_is_strictly_inside(const char *parent, const char *child) {
    size_t parent_length = strlen(parent);
    if (strlen(child) <= parent_length || !same_characters(parent, child, parent_length)) {
        return 0;
    }
    int parent_is_root = parent_length > 0 && is_separator(parent[parent_length - 1]);
    return parent_is_root || is_separator(child[parent_length]);
}

char *runtime_cache_bundle_directory(const char *cache, const char *id, const char *version) {
    const char *segments[2] = {cache, "bundles"};
    char *bundles = runtime_cache_joined(segments, 2);
    int unfit = 0;
    char *folder = bundles == NULL ? NULL : runtime_item_folder(id, &unfit);
    char *versions = folder == NULL ? NULL : runtime_cache_resolve(bundles, (const char *const *)&folder, 1);
    char *directory = versions == NULL ? NULL : runtime_cache_resolve(versions, &version, 1);
    int inside = directory != NULL && id[0] != 0 && version[0] != 0 &&
                 runtime_cache_is_strictly_inside(bundles, versions) &&
                 runtime_cache_is_strictly_inside(versions, directory);
    if (!inside) {
        free(directory);
        directory = NULL;
    }
    free(versions);
    free(folder);
    free(bundles);
    return directory;
}
