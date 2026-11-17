#include "catalog_listed_source.h"

#include "catalog_source.h"

static int is_listed_source(const runtime_catalog_source *source) {
    return source->kind >= RUNTIME_CATALOG_LOCAL && source->kind <= RUNTIME_CATALOG_HTTPS &&
           source->location != NULL && source->location[0] != '\0';
}

int runtime_catalog_are_sources(const runtime_catalog_source *listed, size_t count) {
    if (count > 0 && listed == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_listed_source(&listed[index])) {
            return 0;
        }
    }
    return 1;
}

cJSON *runtime_catalog_listed_source_json(const runtime_catalog_source *source) {
    runtime_catalog_location location;
    location.local = source->kind == RUNTIME_CATALOG_LOCAL;
    location.location = source->location;
    return runtime_catalog_source_json(&location);
}

spisor_status runtime_catalog_add_sources(cJSON *call, const runtime_catalog_source *listed, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(call, "sources");
    for (size_t index = 0; array != NULL && index < count; index++) {
        cJSON *source = runtime_catalog_listed_source_json(&listed[index]);
        if (source == NULL || !cJSON_AddItemToArray(array, source)) {
            cJSON_Delete(source);
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return array == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}
