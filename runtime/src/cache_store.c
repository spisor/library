#include "cache_store.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "directory_tree.h"
#include "json_service.h"
#include "runtime/fetch.h"
#include "sha256.h"

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

/** Whether destination holds the reference's bytes: SPISOR_OK, SPISOR_REFUSED, or SPISOR_OUT_OF_MEMORY asking. */
static spisor_status holds(const char *destination, const file_reference *reference) {
    char actual[RUNTIME_SHA256_HEX_SIZE];
    uint64_t size = 0;
    spisor_status hashed = runtime_sha256_file_sized(destination, actual, &size);
    if (hashed != SPISOR_OK) {
        return hashed == SPISOR_OUT_OF_MEMORY ? hashed : SPISOR_REFUSED;
    }
    return size == reference->size && strcmp(actual, reference->sha256) == 0 ? SPISOR_OK : SPISOR_REFUSED;
}

/** Fetches the reference into destination, its answer in `answer` and its refusal in `refusal`. */
static spisor_status fetched(const char *destination, const file_reference *reference,
                             runtime_fetch_get_answer *answer, spisor_refusal *refusal) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    argument.url = reference->url;
    argument.destination = destination;
    argument.sha256 = reference->sha256;
    argument.limited = 1;
    argument.max_bytes = reference->size;
    return fetch->get(fetch->context, &argument, answer, refusal);
}

static cJSON *wrong_length(const file_reference *reference, uint64_t served) {
    char served_text[32];
    char size_text[32];
    snprintf(served_text, sizeof served_text, "%" PRIu64, served);
    snprintf(size_text, sizeof size_text, "%" PRIu64, reference->size);
    const char *parts[7] = {reference->url, " served ", served_text, " bytes, but the catalog says ", size_text,
                            " with SHA-256 ", reference->sha256};
    return runtime_cache_refusal(parts, 7, CORRUPT_FILE_FIX);
}

static runtime_cache_stored settle(spisor_status status, const runtime_fetch_get_answer *answer,
                                   const spisor_refusal *fetch_refusal, const char *destination,
                                   const file_reference *reference, cJSON **refusal) {
    if (status != SPISOR_OK) {
        *refusal = runtime_refusal_of(fetch_refusal);
        return *refusal == NULL ? RUNTIME_CACHE_OUT_OF_MEMORY : RUNTIME_CACHE_REFUSED;
    }
    if (answer->bytes == reference->size) {
        return RUNTIME_CACHE_FETCHED;
    }
    runtime_remove_file_entry(destination);
    *refusal = wrong_length(reference, answer->bytes);
    return *refusal == NULL ? RUNTIME_CACHE_OUT_OF_MEMORY : RUNTIME_CACHE_REFUSED;
}

runtime_cache_stored runtime_cache_store_file(const cJSON *reference, const char *destination, cJSON **refusal) {
    *refusal = NULL;
    file_reference read = reference_of(reference);
    spisor_status held = holds(destination, &read);
    if (held != SPISOR_REFUSED) {
        return held == SPISOR_OK ? RUNTIME_CACHE_CACHED : RUNTIME_CACHE_OUT_OF_MEMORY;
    }
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal fetch_refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = fetched(destination, &read, &answer, &fetch_refusal);
    runtime_cache_stored stored = settle(status, &answer, &fetch_refusal, destination, &read, refusal);
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &fetch_refusal);
    return stored;
}
