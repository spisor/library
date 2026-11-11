#ifndef RUNTIME_PLACES_H
#define RUNTIME_PLACES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "export.h"

#define RUNTIME_PLACES_OK 0
#define RUNTIME_PLACES_OUT_OF_MEMORY 1
#define RUNTIME_PLACES_NO_LOCAL_APP_DATA 2

typedef struct runtime_places {
    char *portable;
    char *state;
    char *cache;
    char *secrets_file;
    int force_file_store;
} runtime_places;

/**
 * Fills places with the four spisor places for the OS named by windows and macos (neither means
 * Linux), under user_home. An override in environment (SPISOR_HOME, SPISOR_STATE, SPISOR_CACHE,
 * SPISOR_SECRETS) or, on Linux only, XDG_STATE_HOME and XDG_CACHE_HOME, is used only when it is a
 * non-empty absolute path for that OS and is ignored otherwise. Answers RUNTIME_PLACES_OK,
 * RUNTIME_PLACES_OUT_OF_MEMORY, or RUNTIME_PLACES_NO_LOCAL_APP_DATA, leaving places empty on failure.
 *
 * @implNote Windows has no guessed default: with no usable LOCALAPPDATA the machine state and cache
 * cannot be placed, and a made-up directory would split one machine's state across two places.
 */
int runtime_resolve_places(int windows, int macos, const char *user_home, const cJSON *environment,
                           runtime_places *places);

void runtime_free_places(runtime_places *places);

/**
 * The places places.resolve answers for environment with no home given, {home, portable, state, cache,
 * secretsFile, forceFileStore}, home being the user's home environment names; NULL with *refusal set
 * to the {message, fix} places.resolve refuses with, or with *refusal NULL when memory runs out.
 */
cJSON *runtime_places_of_environment(const cJSON *environment, cJSON **refusal);

RUNTIME_EXPORT int32_t runtime_places_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

#endif
