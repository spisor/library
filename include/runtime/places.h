#ifndef RUNTIME_PUBLIC_PLACES_H
#define RUNTIME_PUBLIC_PLACES_H

/**
 * @file
 * runtime's `places` service: the four places spisor keeps its files in on this machine, portable, state, cache and
 * secrets. A size-prefixed struct is read only up to its `size`. Text the caller hands in is NUL-terminated UTF-8
 * borrowed for the call; what an answer or a refusal points at is the provider's until the release for its kind.
 */

#include <stddef.h>
#include <stdint.h>

#include "runtime/definition.h"
#include "runtime/environment.h"
#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * `home` is the user's home, or NULL for the one the environment names (USERPROFILE, else HOME, on Windows; HOME
 * elsewhere). `environment` holds `environment_count` variables; an override (SPISOR_HOME, SPISOR_STATE, SPISOR_CACHE,
 * SPISOR_SECRETS, and on Linux XDG_STATE_HOME and XDG_CACHE_HOME) counts only when it is an absolute path, and
 * Windows needs LOCALAPPDATA for its defaults. First shape ends at `environment_count`.
 */
typedef struct runtime_places_resolve_argument {
    uint32_t size;
    const char *home;
    const runtime_variable *environment;
    size_t environment_count;
} runtime_places_resolve_argument;

/**
 * Every place as an absolute path, with the home they were resolved from. `force_file_store` is 1 when SPISOR_SECRETS
 * chose the secrets folder, so secrets skip the OS credential store. First shape ends at `storage`.
 */
typedef struct runtime_places_answer {
    uint32_t size;
    const char *home;
    const char *portable;
    const char *state;
    const char *cache;
    const char *secrets_file;
    uint32_t force_file_store;
    void *storage;
} runtime_places_answer;

/** The places service. First shape ends at `release_refusal`. */
typedef struct runtime_places_definition {
    uint32_t size;
    void *context;
    /**
     * Resolves every place.
     *
     * @param answer the consumer's empty answer. Required.
     * @param refusal the consumer's empty refusal, or NULL for none.
     * @return OK; MALFORMED for a variable without a name or a value, no home to be found, a home that is not
     * absolute, or on Windows no usable LOCALAPPDATA where a default needs it; OUT_OF_MEMORY.
     */
    spisor_status (*resolve)(void *context, const runtime_places_resolve_argument *argument,
                             runtime_places_answer *answer, spisor_refusal *refusal);
    void (*release_answer)(void *context, runtime_places_answer *answer);
    void (*release_refusal)(void *context, spisor_refusal *refusal);
} runtime_places_definition;

/** The static runtime_places_definition, living until the library is unloaded. */
RUNTIME_DEFINITION_API const void *runtime_places_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
