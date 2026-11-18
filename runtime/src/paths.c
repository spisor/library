#include "paths.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "segment.h"
#include "spisor/path.h"

static char *declared_segment(const cJSON *names, const char *kind) {
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(names, kind);
    return cJSON_IsString(declared) ? runtime_portable_segment(declared->valuestring) : NULL;
}

static char *join_kind(const char *home, const cJSON *names, const char *kind) {
    char *declared = declared_segment(names, kind);
    const char *segments[2] = {home, declared != NULL ? declared : kind};
    char *joined = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_memory_release(declared);
    return joined;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

void runtime_free_paths(runtime_paths *paths) {
    spisor_memory_release(paths->home);
    spisor_memory_release(paths->repos);
    spisor_memory_release(paths->plugin);
    spisor_memory_release(paths->cache);
    spisor_memory_release(paths->config);
    memset(paths, 0, sizeof(*paths));
}

int runtime_resolve_paths(const char *home, const cJSON *names, runtime_paths *paths) {
    paths->home = copy_text(home);
    paths->repos = join_kind(home, names, "repos");
    paths->plugin = join_kind(home, names, "plugin");
    paths->cache = join_kind(home, names, "cache");
    paths->config = join_kind(home, names, "config");
    if (paths->home && paths->repos && paths->plugin && paths->cache && paths->config) {
        return 0;
    }
    runtime_free_paths(paths);
    return 1;
}

int runtime_is_usable_home(const char *home) {
    return home != NULL && home[0] != '\0' && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, home);
}

spisor_status runtime_refuse_home(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, RUNTIME_UNUSABLE_HOME_MESSAGE, RUNTIME_UNUSABLE_HOME_FIX);
}

spisor_status runtime_refuse_portable(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the portable folder is not an absolute path",
                             "send portable as the portable answer of places' resolve, or leave it out when the host "
                             "has no portable layer");
}

char *runtime_plugin_folder(const char *place, const char *folder_name) {
    const char *segments[3] = {place, "plugins", folder_name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}
