#include "catalog_state.h"

#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "catalog_path.h"
#include "catalog_text.h"
#include "catalog_url.h"
#include "naula/sha256.h"
#include "paths.h"
#include "spisor/file.h"
#include "naula/json.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

char *runtime_catalog_state_path(const char *cache, const char *location) {
    naula_sha256 hash;
    char hex[NAULA_SHA256_HEX_SIZE];
    naula_sha256_start(&hash);
    naula_sha256_add(&hash, (const uint8_t *)location, strlen(location));
    naula_sha256_hex(&hash, hex);
    char name[NAULA_SHA256_HEX_SIZE + 5];
    memcpy(name, hex, NAULA_SHA256_HEX_SIZE - 1);
    memcpy(name + NAULA_SHA256_HEX_SIZE - 1, ".json", 6);
    const char *segments[] = {cache, "catalog", name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

char *runtime_catalog_location_of(const runtime_catalog_location *source) {
    return source->local ? runtime_catalog_resolve_path(source->location) : runtime_catalog_copy(source->location);
}

spisor_status runtime_catalog_origin_of(const runtime_catalog_location *source, const char *location, char **origin) {
    if (source->local) {
        return runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, location, origin);
    }
    *origin = runtime_catalog_copy(source->location);
    return *origin == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static char *copied_member(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? runtime_catalog_copy(item->valuestring) : NULL;
}

static int copied_every_member(const cJSON *kept, runtime_catalog_state *state) {
    state->etag = copied_member(kept, "etag");
    state->checked_at = copied_member(kept, "checkedAt");
    state->index = copied_member(kept, "index");
    return (state->etag != NULL || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(kept, "etag"))) &&
           (state->checked_at != NULL || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(kept, "checkedAt"))) &&
           (state->index != NULL || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(kept, "index")));
}

int runtime_catalog_read_state(const char *path, runtime_catalog_state *state) {
    memset(state, 0, sizeof *state);
    size_t length = 0;
    spisor_read_outcome outcome;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    int out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    cJSON *kept = NULL;
    if (bytes != NULL) {
        out_of_memory = naula_json_parse_keeping_last_value(bytes, length, &kept, &(const char *){NULL}) == NAULA_OUT_OF_MEMORY;
    }
    spisor_memory_release(bytes);
    if (cJSON_IsObject(kept) && !copied_every_member(kept, state)) {
        runtime_catalog_free_state(state);
        out_of_memory = 1;
    }
    cJSON_Delete(kept);
    return !out_of_memory;
}

static int add_optional(cJSON *object, const char *name, const char *value) {
    return value == NULL || cJSON_AddStringToObject(object, name, value) != NULL;
}

static char *parent_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = RUNTIME_HOST_IS_WINDOWS ? strrchr(path, '\\') : NULL;
    const char *last = backslash != NULL && (slash == NULL || backslash > slash) ? backslash : slash;
    return last == NULL ? NULL : runtime_catalog_copy_bytes(path, (size_t)(last - path));
}

static spisor_status replace_with_text(const char *path, const char *text) {
    char *parent = parent_of(path);
    size_t length = strlen(text);
    char *bytes = parent == NULL ? NULL : spisor_memory_allocate(length + 2);
    spisor_status status = bytes == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(parent);
    if (status == SPISOR_OK) {
        memcpy(bytes, text, length);
        bytes[length] = '\n';
        status = spisor_replace_file(path, (const uint8_t *)bytes, length + 1);
    }
    spisor_memory_release(bytes);
    spisor_memory_release(parent);
    return status;
}

spisor_status runtime_catalog_write_state(const char *path, const runtime_catalog_state *state) {
    cJSON *object = cJSON_CreateObject();
    int built = object != NULL && add_optional(object, "etag", state->etag) &&
                add_optional(object, "checkedAt", state->checked_at) && add_optional(object, "index", state->index);
    char *text = built ? spisor_json_stringify_pretty(object) : NULL;
    cJSON_Delete(object);
    spisor_status status = text == NULL ? SPISOR_OUT_OF_MEMORY : replace_with_text(path, text);
    spisor_memory_release(text);
    return status;
}

spisor_status runtime_catalog_keep_state(const runtime_catalog_kept_state *kept) {
    return kept->path == NULL ? SPISOR_OK : runtime_catalog_write_state(kept->path, &kept->state);
}

void runtime_catalog_free_kept_state(runtime_catalog_kept_state *kept) {
    spisor_memory_release(kept->path);
    spisor_memory_release(kept->location);
    runtime_catalog_free_state(&kept->state);
    kept->path = NULL;
    kept->location = NULL;
}

void runtime_catalog_free_state(runtime_catalog_state *state) {
    spisor_memory_release(state->etag);
    spisor_memory_release(state->checked_at);
    spisor_memory_release(state->index);
    memset(state, 0, sizeof *state);
}
