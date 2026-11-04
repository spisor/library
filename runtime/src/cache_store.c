#include "cache_store.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "directory_tree.h"
#include "fetch.h"
#include "service_call.h"
#include "sha256.h"
#include "spisor/json_kind.h"

#define CORRUPT_FILE_FIX "never use it: ask the catalog's publisher to correct the file or its entry"

typedef struct file_reference {
    const char *url;
    const char *sha256;
    uint64_t size;
} file_reference;

static file_reference reference_of(const cJSON *reference) {
    file_reference read;
    read.url = cJSON_GetObjectItemCaseSensitive(reference, "url")->valuestring;
    read.sha256 = cJSON_GetObjectItemCaseSensitive(reference, "sha256")->valuestring;
    read.size = (uint64_t)cJSON_GetObjectItemCaseSensitive(reference, "size")->valuedouble;
    return read;
}

static int holds(const char *destination, const file_reference *reference) {
    char actual[RUNTIME_SHA256_HEX_SIZE];
    uint64_t size = 0;
    return runtime_sha256_file_sized(destination, actual, &size) && size == reference->size &&
           strcmp(actual, reference->sha256) == 0;
}

static cJSON *fetch_order(const char *destination, const file_reference *reference) {
    cJSON *order = cJSON_CreateObject();
    int built = order != NULL && cJSON_AddStringToObject(order, "url", reference->url) != NULL &&
                cJSON_AddStringToObject(order, "destination", destination) != NULL &&
                cJSON_AddStringToObject(order, "sha256", reference->sha256) != NULL &&
                cJSON_AddNumberToObject(order, "maxBytes", (double)reference->size) != NULL;
    if (!built) {
        cJSON_Delete(order);
        return NULL;
    }
    return order;
}

/** fetch.get's answer, parsed, with its status in *status; NULL when memory runs out. */
static cJSON *fetched(const char *destination, const file_reference *reference, int32_t *status) {
    cJSON *order = fetch_order(destination, reference);
    cJSON *answer = runtime_call_service(runtime_fetch_get, order, status);
    cJSON_Delete(order);
    return answer;
}

static cJSON *wrong_length(const file_reference *reference, double served) {
    char served_text[32];
    char size_text[32];
    snprintf(served_text, sizeof served_text, "%.0f", served);
    snprintf(size_text, sizeof size_text, "%" PRIu64, reference->size);
    const char *parts[7] = {reference->url, " served ", served_text, " bytes, but the catalog says ", size_text,
                            " with SHA-256 ", reference->sha256};
    return runtime_cache_refusal(parts, 7, CORRUPT_FILE_FIX);
}

static runtime_cache_stored settle(cJSON *answer, int32_t status, const char *destination,
                                   const file_reference *reference, cJSON **refusal) {
    if (answer == NULL) {
        return RUNTIME_CACHE_OUT_OF_MEMORY;
    }
    if (status != SPISOR_STATUS_SUCCESS) {
        *refusal = answer;
        return RUNTIME_CACHE_REFUSED;
    }
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(answer, "bytes");
    double served = cJSON_IsNumber(bytes) ? bytes->valuedouble : -1;
    cJSON_Delete(answer);
    if (served == (double)reference->size) {
        return RUNTIME_CACHE_FETCHED;
    }
    runtime_remove_file_entry(destination);
    *refusal = wrong_length(reference, served);
    return *refusal == NULL ? RUNTIME_CACHE_OUT_OF_MEMORY : RUNTIME_CACHE_REFUSED;
}

runtime_cache_stored runtime_cache_store_file(const cJSON *reference, const char *destination, cJSON **refusal) {
    *refusal = NULL;
    file_reference read = reference_of(reference);
    if (holds(destination, &read)) {
        return RUNTIME_CACHE_CACHED;
    }
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = fetched(destination, &read, &status);
    return settle(answer, status, destination, &read, refusal);
}
