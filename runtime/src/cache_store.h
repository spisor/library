#ifndef RUNTIME_CACHE_STORE_H
#define RUNTIME_CACHE_STORE_H

#include "cJSON.h"

typedef enum runtime_cache_stored {
    RUNTIME_CACHE_CACHED,
    RUNTIME_CACHE_FETCHED,
    RUNTIME_CACHE_REFUSED,
    RUNTIME_CACHE_OUT_OF_MEMORY
} runtime_cache_stored;

/**
 * Makes destination hold the file reference ({url, sha256, size}) names, answering CACHED when it
 * already did and FETCHED when it was fetched through fetch.get. REFUSED sets *refusal to the
 * {message, fix} answer, which the caller frees.
 *
 * @implNote A stored file is hashed on every install and never trusted for existing. The size is handed
 * to fetch.get as maxBytes, so an oversized download stops early; a file shorter than the catalog says
 * is removed again, since bytes carrying that hash can only ever have one length.
 */
runtime_cache_stored runtime_cache_store_file(const cJSON *reference, const char *destination, cJSON **refusal);

#endif
