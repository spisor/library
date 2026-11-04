#include "cache_sweep.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_installed.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "cache_references.h"
#include "cache_refusal.h"
#include "catalog_semver.h"
#include "directory_tree.h"
#include "file_times.h"

#define ABANDONED_AFTER_MILLISECONDS (24.0 * 3600000.0)

typedef struct sweep_scope {
    const char *cache;
    const cJSON *referenced;
    double now;
    cJSON *doomed;
} sweep_scope;

static int is_referenced(const sweep_scope *scope, const char *id, const char *version) {
    const char *parts[3] = {id, "@", version};
    char *name = runtime_cache_text(parts, 3);
    int referenced = name == NULL || cJSON_GetObjectItemCaseSensitive(scope->referenced, name) != NULL;
    free(name);
    return referenced;
}

/** Records id@version (for platform, when given) as one to remove; 0 when memory runs out. */
static int doom(sweep_scope *scope, const char *id, const char *version, const char *platform) {
    cJSON *entry = cJSON_CreateArray();
    int added = entry != NULL && cJSON_AddItemToArray(entry, cJSON_CreateString(id)) &&
                cJSON_AddItemToArray(entry, cJSON_CreateString(version)) &&
                (platform == NULL || cJSON_AddItemToArray(entry, cJSON_CreateString(platform)));
    if (!added || !cJSON_AddItemToArray(scope->doomed, entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

/** semver's newest, the last of equals winning as [...versions].sort(compareVersions).at(-1) does. */
static const char *newest_of(const cJSON *versions) {
    const char *newest = NULL;
    for (const cJSON *version = versions->child; version != NULL; version = version->next) {
        if (newest == NULL || runtime_catalog_compare_versions(version->valuestring, newest) >= 0) {
            newest = version->valuestring;
        }
    }
    return newest;
}

static int doom_unkept(sweep_scope *scope, const cJSON *installed, const char *platform) {
    int doomed = installed != NULL;
    for (const cJSON *id = doomed ? installed->child : NULL; doomed && id != NULL; id = id->next) {
        const char *rollback = newest_of(id);
        for (const cJSON *version = id->child; doomed && version != NULL; version = version->next) {
            if (strcmp(version->valuestring, rollback) != 0 && !is_referenced(scope, id->string, version->valuestring)) {
                doomed = doom(scope, id->string, version->valuestring, platform);
            }
        }
    }
    return doomed;
}

static int is_abandoned(const sweep_scope *scope, const char *const *segments, size_t count) {
    char *directory = runtime_cache_joined(segments, count);
    double newest = 0;
    int abandoned = directory != NULL && !runtime_cache_has_marker(directory) &&
                    runtime_newest_change(directory, &newest) && scope->now - newest > ABANDONED_AFTER_MILLISECONDS;
    free(directory);
    return abandoned;
}

/** Dooms each unmarked folder under the segments a day untouched and not referenced; depth 2 for bundles, 3 for libraries. */
static int doom_abandoned(sweep_scope *scope, const char **segments, size_t count, size_t depth) {
    cJSON *names = runtime_cache_folder_names(segments, count);
    int doomed = names != NULL;
    for (const cJSON *name = doomed ? names->child : NULL; doomed && name != NULL; name = name->next) {
        segments[count] = name->valuestring;
        if (count + 1 < depth + 2) {
            doomed = doom_abandoned(scope, segments, count + 1, depth);
        } else if (is_abandoned(scope, segments, count + 1) && !is_referenced(scope, segments[2], segments[3])) {
            doomed = doom(scope, segments[2], segments[3], depth == 3 ? segments[4] : NULL);
        }
    }
    cJSON_Delete(names);
    return doomed;
}

static int doom_bundles(sweep_scope *scope) {
    cJSON *installed = runtime_cache_marked_bundles(scope->cache);
    int doomed = doom_unkept(scope, installed, NULL);
    cJSON_Delete(installed);
    const char *segments[5] = {scope->cache, "bundles", NULL, NULL, NULL};
    return doomed && doom_abandoned(scope, segments, 2, 2);
}

static int doom_libraries(sweep_scope *scope) {
    cJSON *platforms = runtime_cache_library_platforms(scope->cache);
    int doomed = platforms != NULL;
    for (const cJSON *platform = doomed ? platforms->child : NULL; doomed && platform != NULL; platform = platform->next) {
        cJSON *installed = runtime_cache_marked_libraries(scope->cache, platform->valuestring);
        doomed = doom_unkept(scope, installed, platform->valuestring);
        cJSON_Delete(installed);
    }
    cJSON_Delete(platforms);
    const char *segments[5] = {scope->cache, "libraries", NULL, NULL, NULL};
    return doomed && doom_abandoned(scope, segments, 2, 3);
}

/** Resolves each step from root, checking each lies strictly inside the one before; NULL when one escapes. */
static char *contained(const char *root, const char *const *steps, size_t count, char **parent) {
    char *at = runtime_cache_resolve(root, NULL, 0);
    *parent = NULL;
    for (size_t index = 0; at != NULL && index < count; index++) {
        char *next = runtime_cache_resolve(at, &steps[index], 1);
        int inside = next != NULL && runtime_cache_is_strictly_inside(at, next);
        free(*parent);
        *parent = at;
        at = inside ? next : NULL;
        if (!inside) {
            free(next);
        }
    }
    return at;
}

/** @implNote A version that cannot be removed (a file another process holds open on Windows) is skipped for the next sweep. */
static int remove_doomed(const char *cache, const cJSON *entry) {
    const char *steps[3] = {cJSON_GetArrayItem(entry, 0)->valuestring, cJSON_GetArrayItem(entry, 1)->valuestring,
                            cJSON_GetArraySize(entry) == 3 ? cJSON_GetArrayItem(entry, 2)->valuestring : NULL};
    size_t count = steps[2] == NULL ? 2 : 3;
    const char *root_segments[2] = {cache, count == 3 ? "libraries" : "bundles"};
    char *root = runtime_cache_joined(root_segments, 2);
    char *parent = NULL;
    char *directory = root == NULL ? NULL : contained(root, steps, count, &parent);
    int removed = directory != NULL && runtime_remove_tree(directory);
    if (removed && count == 3) {
        runtime_remove_empty_directory(parent);
    }
    free(directory);
    free(parent);
    free(root);
    return removed;
}

static cJSON *removed_name(const cJSON *entry) {
    const char *parts[5] = {cJSON_GetArrayItem(entry, 0)->valuestring, "@", cJSON_GetArrayItem(entry, 1)->valuestring,
                            " for ", cJSON_GetArraySize(entry) == 3 ? cJSON_GetArrayItem(entry, 2)->valuestring : ""};
    char *name = runtime_cache_text(parts, cJSON_GetArraySize(entry) == 3 ? 5 : 3);
    cJSON *removed = name == NULL ? NULL : cJSON_CreateString(name);
    free(name);
    return removed;
}

static cJSON *remove_all(const char *cache, const cJSON *doomed) {
    cJSON *removed = cJSON_CreateArray();
    for (const cJSON *entry = removed == NULL ? NULL : doomed->child; entry != NULL; entry = entry->next) {
        if (remove_doomed(cache, entry) && !cJSON_AddItemToArray(removed, removed_name(entry))) {
            cJSON_Delete(removed);
            return NULL;
        }
    }
    return removed;
}

cJSON *runtime_cache_sweep_versions(const char *cache, const char *state) {
    double now = runtime_now_milliseconds();
    cJSON *referenced = runtime_cache_referenced(cache, state, now);
    sweep_scope scope = {cache, referenced, now, cJSON_CreateArray()};
    cJSON *removed = NULL;
    if (referenced != NULL && scope.doomed != NULL && doom_bundles(&scope) && doom_libraries(&scope)) {
        removed = remove_all(cache, scope.doomed);
    }
    cJSON_Delete(scope.doomed);
    cJSON_Delete(referenced);
    return removed;
}
