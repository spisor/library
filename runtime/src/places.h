#ifndef RUNTIME_PLACES_H
#define RUNTIME_PLACES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "runtime/places.h"

#define RUNTIME_NO_LOCAL_APP_DATA_MESSAGE "LOCALAPPDATA is absent, empty or not an absolute path"
#define RUNTIME_NO_LOCAL_APP_DATA_FIX                                                                   \
    "send the environment's LOCALAPPDATA as an absolute directory path; Windows has no default for the " \
    "machine state"

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
 * Resolves the places of a JSON environment through the places definition, the home being the one it names, for a
 * caller that holds its environment as JSON. The answer is the consumer's empty answer, released with the
 * definition's release_answer. 1, or 0 with *refusal set to the {message, fix} refusal, or left NULL when memory ran
 * out.
 */
int runtime_resolve_places_answer(const cJSON *environment, runtime_places_answer *answer, cJSON **refusal);

/**
 * The places places.resolve answers for environment with no home given, {home, portable, state, cache,
 * secretsFile, forceFileStore}, home being the user's home environment names; NULL with *refusal set
 * to the {message, fix} places.resolve refuses with, or with *refusal NULL when memory runs out.
 */
cJSON *runtime_places_of_environment(const cJSON *environment, cJSON **refusal);

#endif
