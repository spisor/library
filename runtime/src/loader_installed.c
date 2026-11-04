#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "host_references.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define INSTALLED_FIX "install the host bundle again, so its host.json carries a loader standard"

static cJSON *refusal_naming(const char *file, const char *what, const char *issue_path, const char *detail) {
    size_t size = strlen(file) + strlen(what) + strlen(issue_path) + strlen(detail) + 4;
    char *message = malloc(size);
    cJSON *refusal = NULL;
    if (message != NULL) {
        snprintf(message, size, "%s %s%s%s%s", file, what, issue_path, issue_path[0] == '\0' ? "" : " ", detail);
        refusal = runtime_refusal(message, INSTALLED_FIX);
    }
    free(message);
    return refusal;
}

static cJSON *standard_answer(cJSON *standard) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || !cJSON_AddItemToObject(answer, "standard", standard)) {
        cJSON_Delete(answer);
        cJSON_Delete(standard);
        return NULL;
    }
    return answer;
}

static cJSON *checked_standard(const char *file, cJSON *manifest, cJSON **refusal) {
    cJSON *issues = runtime_loader_standard_issues(manifest);
    const cJSON *first = cJSON_GetArrayItem(issues, 0);
    if (issues != NULL && first == NULL) {
        cJSON_Delete(issues);
        return standard_answer(manifest);
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
    uint8_t *bytes = spisor_read_file(file, &length);
    int readable = bytes != NULL;
    cJSON *manifest = readable ? spisor_parse_json(bytes, length) : NULL;
    free(bytes);
    if (manifest == NULL) {
        *refusal = refusal_naming(file, "could not be read: ", "",
                                  readable ? "it is not JSON" : "the file is missing or unreadable");
        return NULL;
    }
    return checked_standard(file, manifest, refusal);
}

static int is_plain(const char *text) {
    return text != NULL && runtime_is_plain_host_id(text);
}

static cJSON *installed_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *cache = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "places"), "cache");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "id"));
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "version"));
    if (!cJSON_IsString(cache) || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache->valuestring) ||
        !is_plain(id) || !is_plain(version)) {
        *refusal = runtime_refusal("the argument carries no usable places.cache, id or version",
                                   "send {\"places\": {\"cache\": \"<absolute directory>\"}, \"id\": \"<host id>\", "
                                   "\"version\": \"<version>\"}, the id and version plain file names");
        return NULL;
    }
    const char *segments[5] = {cache->valuestring, "bundles", id, version, "host.json"};
    char *file = runtime_cache_joined(segments, 5);
    cJSON *answer = file == NULL ? NULL : read_standard(file, refusal);
    free(file);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_loader_installed(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, installed_for);
}
