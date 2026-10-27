#include "paths.h"

#include <stdlib.h>
#include <string.h>

#include "segment.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#if defined(_WIN32)
#define RUNTIME_HOST_IS_WINDOWS 1
#else
#define RUNTIME_HOST_IS_WINDOWS 0
#endif

static char *declared_segment(const cJSON *names, const char *kind) {
    const cJSON *declared = cJSON_GetObjectItemCaseSensitive(names, kind);
    return cJSON_IsString(declared) ? runtime_portable_segment(declared->valuestring) : NULL;
}

static char *join_kind(const char *home, const cJSON *names, const char *kind) {
    char *declared = declared_segment(names, kind);
    const char *segments[2] = {home, declared != NULL ? declared : kind};
    char *joined = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    free(declared);
    return joined;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

void runtime_free_paths(runtime_paths *paths) {
    free(paths->home);
    free(paths->repos);
    free(paths->plugin);
    free(paths->cache);
    free(paths->config);
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

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int add_path(cJSON *result, const char *kind, const char *path) {
    return cJSON_AddStringToObject(result, kind, path) != NULL;
}

static cJSON *paths_to_json(const runtime_paths *paths) {
    cJSON *result = cJSON_CreateObject();
    int complete = result != NULL && add_path(result, "home", paths->home) && add_path(result, "repos", paths->repos) &&
                   add_path(result, "plugin", paths->plugin) && add_path(result, "cache", paths->cache) &&
                   add_path(result, "config", paths->config);
    if (!complete) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static int32_t answer_paths(const char *home, const cJSON *names, uint8_t **out, size_t *out_len) {
    runtime_paths paths;
    if (runtime_resolve_paths(home, names, &paths) != 0) {
        return out_of_memory(out, out_len);
    }
    cJSON *result = paths_to_json(&paths);
    runtime_free_paths(&paths);
    if (result == NULL) {
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

RUNTIME_EXPORT int32_t runtime_paths_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(call.argument, "home");
    if (!cJSON_IsString(home)) {
        status = spisor_fail("the argument carries no string home", "send {\"home\": \"<directory>\"}", out, out_len);
    } else {
        status = answer_paths(home->valuestring, cJSON_GetObjectItemCaseSensitive(call.argument, "names"), out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}
