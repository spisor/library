#include "loader.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "host_references.h"
#include "spisor/json_kind.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define INSTALLED_FIX "install the host bundle again, so its host.json carries a loader standard"

static cJSON *refusal_naming(const char *file, const char *what, const char *issue_path, const char *detail) {
    size_t size = strlen(file) + strlen(what) + strlen(issue_path) + strlen(detail) + 4;
    char *message = spisor_memory_allocate(size);
    cJSON *refusal = NULL;
    if (message != NULL) {
        snprintf(message, size, "%s %s%s%s%s", file, what, issue_path, issue_path[0] == '\0' ? "" : " ", detail);
        refusal = runtime_refusal(message, INSTALLED_FIX);
    }
    spisor_memory_release(message);
    return refusal;
}

static cJSON *checked_standard(const char *file, cJSON *manifest, cJSON **refusal) {
    cJSON *issues = runtime_loader_standard_issues(manifest);
    const cJSON *first = cJSON_GetArrayItem(issues, 0);
    if (issues != NULL && first == NULL) {
        cJSON_Delete(issues);
        return manifest;
    }
    if (first != NULL) {
        *refusal = refusal_naming(file, "is not a loader standard: ",
                                  cJSON_GetObjectItemCaseSensitive(first, "path")->valuestring,
                                  cJSON_GetObjectItemCaseSensitive(first, "detail")->valuestring);
    }
    cJSON_Delete(issues);
    cJSON_Delete(manifest);
    return NULL;
}

static cJSON *read_standard(const char *file, cJSON **refusal) {
    size_t length = 0;
    spisor_read_outcome outcome;
    uint8_t *bytes = spisor_read_file_reporting(file, &length, &outcome);
    int readable = bytes != NULL;
    int out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    cJSON *manifest = readable ? spisor_parse_json_reporting(bytes, length, &out_of_memory) : NULL;
    spisor_memory_release(bytes);
    if (manifest == NULL && !out_of_memory) {
        *refusal = refusal_naming(file, "could not be read: ", "",
                                  readable ? "it is not JSON" : "the file is missing or unreadable");
        return NULL;
    }
    return manifest == NULL ? NULL : checked_standard(file, manifest, refusal);
}

static int is_plain(const char *text) {
    return text != NULL && runtime_is_plain_host_id(text);
}

cJSON *runtime_loader_installed(const char *cache, const char *id, const char *version, cJSON **refusal) {
    *refusal = NULL;
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache) || !is_plain(id) ||
        !is_plain(version)) {
        *refusal = runtime_refusal("the call carries no usable cache place, id or version",
                                   "pass the cache place as an absolute directory, and the id and version as plain "
                                   "file names");
        return NULL;
    }
    const char *segments[5] = {cache, "bundles", id, version, "host.json"};
    char *file = runtime_cache_joined(segments, 5);
    cJSON *standard = file == NULL ? NULL : read_standard(file, refusal);
    spisor_memory_release(file);
    return standard;
}
