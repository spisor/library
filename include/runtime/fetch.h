#ifndef RUNTIME_PUBLIC_FETCH_H
#define RUNTIME_PUBLIC_FETCH_H

/**
 * @file
 * runtime's `fetch` service: fetches one https: or file: url into a file, verified before it is put in place. A
 * size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8 borrowed for the
 * call; what an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stdint.h>

#include "runtime/definition.h"
#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What a fetch did. Append-only: a published value keeps its number. */
typedef enum runtime_fetch_get_outcome {
    /** The bytes are at the destination. */
    RUNTIME_FETCH_GET_STORED = 1,
    /** The server answered 304 to the etag or last_modified sent, and nothing was written. */
    RUNTIME_FETCH_GET_NOT_MODIFIED = 2
} runtime_fetch_get_outcome;

/**
 * One fetch. `url` is https: or file:; `destination` an absolute path; `sha256` 64 lowercase hex digits the bytes must
 * hash to, or NULL; `etag` and `last_modified` the header values a previous answer carried, or NULL; `max_bytes` the
 * most bytes accepted, read only when `limited` is 1; `timeout_seconds` above zero and at most a day, or 0 for the
 * default of 75 seconds. First shape ends at `timeout_seconds`.
 */
typedef struct runtime_fetch_get_argument {
    uint32_t size;
    const char *url;
    const char *destination;
    const char *sha256;
    const char *etag;
    const char *last_modified;
    uint32_t limited;
    uint64_t max_bytes;
    double timeout_seconds;
} runtime_fetch_get_argument;

/**
 * What a fetch did, a runtime_fetch_get_outcome. For STORED, `sha256` is the bytes' digest, `bytes` their count, and
 * `etag` and `last_modified` what the server sent, each NULL when it sent none; for NOT_MODIFIED every other field is
 * empty.
 * First shape ends at `storage`.
 */
typedef struct runtime_fetch_get_answer {
    uint32_t size;
    uint32_t outcome;
    const char *sha256;
    uint64_t bytes;
    const char *etag;
    const char *last_modified;
    void *storage;
} runtime_fetch_get_answer;

/** The fetch service. First shape ends at `release_refusal`. */
typedef struct runtime_fetch_definition {
    uint32_t size;
    void *context;
    /**
     * Fetches the url into the destination. The bytes go to a partial file beside it and are renamed into place only
     * once complete, within `max_bytes`, and matching `sha256`, so a refusal leaves neither the destination nor the
     * partial file behind. A file: url is copied through the same path, so a local catalog and a published one are
     * verified alike.
     *
     * @param answer the consumer's empty answer. Required.
     * @param refusal the consumer's empty refusal, or NULL for none.
     * @return OK; MALFORMED for an empty url, a destination that is not absolute, a sha256 that is not 64 lowercase
     * hex digits, an etag or last_modified holding a control character, or a timeout out of range; REFUSED for a
     * scheme the cache does not fetch, an answer other than 200 or an asked-for 304, a cut-off or oversized body, a
     * digest other than `sha256`, or a destination that cannot be written; OUT_OF_MEMORY.
     */
    spisor_status (*get)(void *context, const runtime_fetch_get_argument *argument, runtime_fetch_get_answer *answer,
                         spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_fetch_get_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_fetch_definition;

/** The static runtime_fetch_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_fetch_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
