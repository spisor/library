#include "catalog_state.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_path.h"
#include "catalog_text.h"
#include "catalog_url.h"
#include "paths.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

char *runtime_catalog_state_path(const char *cache, const char *location) {
    runtime_sha256 hash;
    char hex[RUNTIME_SHA256_HEX_SIZE];
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)location, strlen(location));
    runtime_sha256_finish_hex(&hash, hex);
    char name[RUNTIME_SHA256_HEX_SIZE + 5];
    memcpy(name, hex, RUNTIME_SHA256_HEX_SIZE - 1);
    memcpy(name + RUNTIME_SHA256_HEX_SIZE - 1, ".json", 6);
    const char *segments[] = {cache, "catalog", name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

char *runtime_catalog_location_of(const runtime_catalog_source *source) {
    return source->local ? runtime_catalog_resolve_path(source->location) : runtime_catalog_copy(source->location);
}

char *runtime_catalog_origin_of(const runtime_catalog_source *source, const char *location) {
    return source->local ? runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, location)
                         : runtime_catalog_copy(source->location);
}

static char *copied_member(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? runtime_catalog_copy(item->valuestring) : NULL;
}

void runtime_catalog_read_state(const char *path, runtime_catalog_state *state) {
    memset(state, 0, sizeof *state);
    size_t length;
    uint8_t *bytes = spisor_read_file(path, &length);
    cJSON *kept = bytes == NULL ? NULL : spisor_parse_json(bytes, length);
    free(bytes);
    if (cJSON_IsObject(kept)) {
        state->etag = copied_member(kept, "etag");
        state->checked_at = copied_member(kept, "checkedAt");
        state->index = copied_member(kept, "index");
    }
    cJSON_Delete(kept);
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

static int replace_with_text(const char *path, const char *text) {
    char *parent = parent_of(path);
    int written = parent != NULL && spisor_make_directories(parent);
    free(parent);
    if (!written) {
        return 0;
    }
    size_t length = strlen(text);
    char *bytes = malloc(length + 2);
    if (bytes == NULL) {
        return 0;
    }
    memcpy(bytes, text, length);
    bytes[length] = '\n';
    written = spisor_replace_file(path, (const uint8_t *)bytes, length + 1);
    free(bytes);
    return written;
}

int runtime_catalog_write_state(const char *path, const runtime_catalog_state *state) {
    cJSON *object = cJSON_CreateObject();
    int built = object != NULL && add_optional(object, "etag", state->etag) &&
                add_optional(object, "checkedAt", state->checked_at) && add_optional(object, "index", state->index);
    char *text = built ? spisor_json_stringify_pretty(object) : NULL;
    cJSON_Delete(object);
    int written = text != NULL && replace_with_text(path, text);
    free(text);
    return written;
}

void runtime_catalog_free_state(runtime_catalog_state *state) {
    free(state->etag);
    free(state->checked_at);
    free(state->index);
    memset(state, 0, sizeof *state);
}
