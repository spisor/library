#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cache_marker.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_place.h"
#include "cache_refusal.h"
#include "catalog_text.h"
#include "file_stream.h"
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

static int names_present_files(const char *directory, const cJSON *hashes) {
    const char *none = NULL;
    char *resolved_directory = runtime_cache_resolve(directory, &none, 0);
    int present = resolved_directory != NULL;
    for (const cJSON *member = present ? hashes->child : NULL; present && member != NULL; member = member->next) {
        const char *key = member->string;
        char *path = runtime_cache_resolve(resolved_directory, &key, 1);
        present = path != NULL && runtime_cache_is_strictly_inside(resolved_directory, path) && spisor_file_exists(path);
        free(path);
    }
    free(resolved_directory);
    return present;
}

int runtime_cache_has_files(const char *directory) {
    char *path = marker_path(directory);
    cJSON *hashes = path == NULL ? NULL : recorded_hashes(path);
    free(path);
    int present = hashes != NULL && names_present_files(directory, hashes);
    cJSON_Delete(hashes);
    return present;
}

static int add_copy(cJSON *marker, const char *name, const cJSON *value) {
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (copy == NULL || !cJSON_AddItemToObject(marker, name, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

static char *marker_text(const cJSON *hashes, const cJSON *sizes) {
    cJSON *marker = cJSON_CreateObject();
    if (marker == NULL || !add_copy(marker, "files", hashes) || !add_copy(marker, "sizes", sizes)) {
        cJSON_Delete(marker);
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

int runtime_cache_write_marker(const char *directory, const cJSON *hashes, const cJSON *sizes) {
    char *path = marker_path(directory);
    char *text = path == NULL ? NULL : marker_text(hashes, sizes);
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

/** The size of the file at path in bytes in *size; 0 when it cannot be opened. */
static int file_size(const char *path, double *size) {
    FILE *file = runtime_open_file(path, RUNTIME_FILE_READ);
    if (file == NULL) {
        return 0;
    }
#if defined(_WIN32)
    long long end = _fseeki64(file, 0, SEEK_END) == 0 ? _ftelli64(file) : -1;
#else
    long long end = fseeko(file, 0, SEEK_END) == 0 ? (long long)ftello(file) : -1;
#endif
    fclose(file);
    *size = (double)end;
    return end >= 0;
}

static file_verdict unsized_verdict(const char *directory, const char *key) {
    const char *parts[4] = {"the completion marker of ", directory, " records no size for ", key};
    return verdict_of(damaged(runtime_cache_text(parts, 4), key));
}

static file_verdict resized_verdict(const char *directory, const char *key, double actual, double recorded) {
    char actual_text[32];
    char recorded_text[32];
    snprintf(actual_text, sizeof actual_text, "%.0f", actual);
    snprintf(recorded_text, sizeof recorded_text, "%.0f", recorded);
    const char *parts[8] = {"file ", key, " of ", directory, " has ", actual_text, " bytes, but its install recorded ",
                            recorded_text};
    return verdict_of(damaged(runtime_cache_text(parts, 8), key));
}

static file_verdict check_size(const char *directory, const char *resolved_directory, const char *key, const cJSON *sizes) {
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(sizes, key);
    if (!cJSON_IsNumber(recorded)) {
        return unsized_verdict(directory, key);
    }
    char *path = runtime_cache_resolve(resolved_directory, &key, 1);
    file_verdict verdict = {NULL, path == NULL};
    double actual = 0;
    if (path == NULL) {
        return verdict;
    }
    if (strcmp(key, RUNTIME_INSTALL_MARKER) == 0 || !runtime_cache_is_strictly_inside(resolved_directory, path)) {
        verdict = outside_verdict(directory, key);
    } else if (!file_size(path, &actual)) {
        verdict = missing_verdict(directory, key);
    } else if (actual != recorded->valuedouble) {
        verdict = resized_verdict(directory, key, actual, recorded->valuedouble);
    }
    free(path);
    return verdict;
}

static cJSON *check_sizes(const char *directory, const cJSON *hashes, const cJSON *sizes) {
    const char *none = NULL;
    char *resolved_directory = runtime_cache_resolve(directory, &none, 0);
    file_verdict verdict = {NULL, resolved_directory == NULL};
    for (const cJSON *member = resolved_directory == NULL ? NULL : hashes->child;
         member != NULL && verdict.damaged == NULL && !verdict.out_of_memory; member = member->next) {
        verdict = check_size(directory, resolved_directory, member->string, sizes);
    }
    free(resolved_directory);
    return verdict.out_of_memory ? NULL : verdict.damaged != NULL ? verdict.damaged : kind_only("intact");
}

cJSON *runtime_cache_check_version_sizes(const char *directory) {
    char *path = marker_path(directory);
    if (path == NULL) {
        return NULL;
    }
    if (!spisor_file_exists(path)) {
        free(path);
        return kind_only("absent");
    }
    cJSON *marker = runtime_read_node_json(path);
    const cJSON *files = cJSON_IsObject(marker) ? cJSON_GetObjectItemCaseSensitive(marker, "files") : NULL;
    cJSON *answer;
    if (!is_hash_map(files)) {
        const char *parts[3] = {"the completion marker ", path, " is not readable"};
        answer = damaged(runtime_cache_text(parts, 3), NULL);
    } else {
        answer = check_sizes(directory, files, cJSON_GetObjectItemCaseSensitive(marker, "sizes"));
    }
    cJSON_Delete(marker);
    free(path);
    return answer;
}
