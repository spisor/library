#include "catalog_source.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_text.h"

#define NOT_AN_OBJECT "a catalog source must be an object with a kind"

static char *kind_problem(const cJSON *kind) {
    char *written = kind == NULL ? runtime_catalog_copy("undefined") : cJSON_PrintUnformatted(kind);
    if (written == NULL) {
        return NULL;
    }
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add(&text, "unknown or incomplete catalog source kind ");
    runtime_catalog_text_add(&text, written);
    if (kind == NULL) {
        free(written);
    } else {
        cJSON_free(written);
    }
    return runtime_catalog_text_finish(&text);
}

static const char *string_member(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

int runtime_catalog_source_of(const cJSON *entry, runtime_catalog_source *source, char **problem) {
    *problem = NULL;
    if (!cJSON_IsObject(entry)) {
        *problem = runtime_catalog_copy(NOT_AN_OBJECT);
        return 0;
    }
    const char *kind = string_member(entry, "kind");
    const char *path = string_member(entry, "path");
    const char *url = string_member(entry, "url");
    if (kind != NULL && strcmp(kind, "local") == 0 && path != NULL) {
        source->local = 1;
        source->location = path;
        return 1;
    }
    if (kind != NULL && strcmp(kind, "https") == 0 && url != NULL) {
        source->local = 0;
        source->location = url;
        return 1;
    }
    *problem = kind_problem(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
    return 0;
}

cJSON *runtime_catalog_source_json(const runtime_catalog_source *source) {
    cJSON *written = cJSON_CreateObject();
    int built = written != NULL && cJSON_AddStringToObject(written, "kind", source->local ? "local" : "https") != NULL &&
                cJSON_AddStringToObject(written, source->local ? "path" : "url", source->location) != NULL;
    if (!built) {
        cJSON_Delete(written);
        return NULL;
    }
    return written;
}

int runtime_catalog_sources_of(const cJSON *array, runtime_catalog_source **sources, size_t *count, char **problem) {
    *problem = NULL;
    *count = 0;
    *sources = malloc(((size_t)cJSON_GetArraySize(array) + 1) * sizeof **sources);
    if (*sources == NULL) {
        return 0;
    }
    for (const cJSON *entry = array->child; entry != NULL; entry = entry->next) {
        if (!runtime_catalog_source_of(entry, &(*sources)[*count], problem)) {
            free(*sources);
            *sources = NULL;
            return 0;
        }
        (*count)++;
    }
    return 1;
}
