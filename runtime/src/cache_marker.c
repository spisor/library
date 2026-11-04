#include "cache_marker.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cache_place.h"
#include "cache_refusal.h"
#include "catalog_text.h"
#include "node_json_file.h"
#include "paths.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define REINSTALL_FIX "install the version again; the cache refetches every file whose hash does not match"

typedef struct file_verdict {
    cJSON *damaged;
    int out_of_memory;
} file_verdict;

static char *marker_path(const char *directory) {
    const char *segments[2] = {directory, RUNTIME_INSTALL_MARKER};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static int is_sha256(const char *text) {
    size_t length = 0;
    for (; text[length] != 0; length++) {
        char digit = text[length];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return 0;
        }
    }
    return length == 64;
}

static int is_hash_map(const cJSON *files) {
    if (!cJSON_IsObject(files)) {
        return 0;
    }
    for (const cJSON *member = files->child; member != NULL; member = member->next) {
        if (!cJSON_IsString(member) || !is_sha256(member->valuestring)) {
            return 0;
        }
    }
    return 1;
}

/** The marker's files member, detached, or NULL when the marker is missing or not a map of hashes. */
static cJSON *recorded_hashes(const char *path) {
    cJSON *marker = runtime_read_node_json(path);
    cJSON *files = cJSON_IsObject(marker) ? cJSON_GetObjectItemCaseSensitive(marker, "files") : NULL;
    cJSON *hashes = is_hash_map(files) ? cJSON_DetachItemViaPointer(marker, files) : NULL;
    cJSON_Delete(marker);
    return hashes;
}

int runtime_cache_has_marker(const char *directory) {
    char *path = marker_path(directory);
    cJSON *hashes = path == NULL ? NULL : recorded_hashes(path);
    free(path);
    int present = hashes != NULL;
    cJSON_Delete(hashes);
    return present;
}

static char *marker_text(const cJSON *hashes) {
    cJSON *marker = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(hashes, 1);
    if (marker == NULL || copy == NULL || !cJSON_AddItemToObject(marker, "files", copy)) {
        cJSON_Delete(marker);
        cJSON_Delete(copy);
        return NULL;
    }
    char *pretty = spisor_json_stringify_pretty(marker);
    cJSON_Delete(marker);
    if (pretty == NULL) {
        return NULL;
    }
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, pretty);
    runtime_catalog_text_add_character(&text, '\n');
    free(pretty);
    return runtime_catalog_text_finish(&text);
}

static int holds_text(const char *path, const char *text) {
    size_t length = 0;
    uint8_t *present = spisor_read_file(path, &length);
    int same = present != NULL && length == strlen(text) && memcmp(present, text, length) == 0;
    free(present);
    return same;
}

int runtime_cache_write_marker(const char *directory, const cJSON *hashes) {
    char *path = marker_path(directory);
    char *text = path == NULL ? NULL : marker_text(hashes);
    int written =
        text != NULL && (spisor_replace_file(path, (const uint8_t *)text, strlen(text)) || holds_text(path, text));
    free(text);
    free(path);
    return written;
}

static cJSON *kind_only(const char *kind) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddStringToObject(answer, "kind", kind) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

/** The damaged answer, taking detail, which it frees; NULL when memory runs out. */
static cJSON *damaged(char *detail, const char *file) {
    cJSON *answer = detail == NULL ? NULL : kind_only("damaged");
    int built = answer != NULL && cJSON_AddStringToObject(answer, "detail", detail) != NULL &&
                cJSON_AddStringToObject(answer, "fix", REINSTALL_FIX) != NULL &&
                (file == NULL || cJSON_AddStringToObject(answer, "file", file) != NULL);
    free(detail);
    if (!built) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static file_verdict verdict_of(cJSON *damaged_answer) {
    file_verdict verdict = {damaged_answer, damaged_answer == NULL};
    return verdict;
}

static file_verdict outside_verdict(const char *directory, const char *key) {
    const char *parts[4] = {"the completion marker names ", key, ", which is not a file of ", directory};
    return verdict_of(damaged(runtime_cache_text(parts, 4), key));
}

static file_verdict missing_verdict(const char *directory, const char *key) {
    const char *parts[5] = {"file ", key, " of ", directory, " is missing"};
    return verdict_of(damaged(runtime_cache_text(parts, 5), key));
}

static file_verdict changed_verdict(const char *directory, const char *key, const char *actual, const char *recorded) {
    const char *parts[8] = {"file ", key, " of ", directory, " has SHA-256 ", actual, ", but its install recorded ",
                            recorded};
    return verdict_of(damaged(runtime_cache_text(parts, 8), key));
}

static file_verdict check_file(const char *directory, const char *resolved_directory, const cJSON *recorded) {
    const char *key = recorded->string;
    char *path = runtime_cache_resolve(resolved_directory, &key, 1);
    file_verdict verdict = {NULL, path == NULL};
    char actual[RUNTIME_SHA256_HEX_SIZE];
    if (path == NULL) {
        return verdict;
    }
    if (strcmp(key, RUNTIME_INSTALL_MARKER) == 0 || !runtime_cache_is_strictly_inside(resolved_directory, path)) {
        verdict = outside_verdict(directory, key);
    } else if (!spisor_sha256_file(path, actual)) {
        verdict = missing_verdict(directory, key);
    } else if (strcmp(actual, recorded->valuestring) != 0) {
        verdict = changed_verdict(directory, key, actual, recorded->valuestring);
    }
    free(path);
    return verdict;
}

static cJSON *check_recorded(const char *directory, const cJSON *hashes) {
    const char *none = NULL;
    char *resolved_directory = runtime_cache_resolve(directory, &none, 0);
    size_t count = 0;
    const cJSON **members = resolved_directory == NULL ? NULL : spisor_json_members_in_javascript_order(hashes, &count);
    cJSON *answer = NULL;
    if (resolved_directory != NULL && (members != NULL || count == 0)) {
        file_verdict verdict = {NULL, 0};
        for (size_t index = 0; index < count && verdict.damaged == NULL && !verdict.out_of_memory; index++) {
            verdict = check_file(directory, resolved_directory, members[index]);
        }
        answer = verdict.out_of_memory ? NULL : verdict.damaged != NULL ? verdict.damaged : kind_only("intact");
    }
    free((void *)members);
    free(resolved_directory);
    return answer;
}

cJSON *runtime_cache_check_version(const char *directory) {
    char *path = marker_path(directory);
    if (path == NULL) {
        return NULL;
    }
    if (!spisor_file_exists(path)) {
        free(path);
        return kind_only("absent");
    }
    cJSON *hashes = recorded_hashes(path);
    cJSON *answer;
    if (hashes == NULL) {
        const char *parts[3] = {"the completion marker ", path, " is not readable"};
        answer = damaged(runtime_cache_text(parts, 3), NULL);
    } else {
        answer = check_recorded(directory, hashes);
    }
    cJSON_Delete(hashes);
    free(path);
    return answer;
}
