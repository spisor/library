#include <string.h>

#include "answer_storage.h"
#include "cache_place.h"
#include "operations.h"
#include "paths.h"
#include "runtime/places.h"
#include "spisor/allocation.h"
#include "spisor/definition.h"
#include "user_home.h"

#define ARGUMENT_SHAPE SPISOR_SHAPE_THROUGH(runtime_testing_real_places_argument, environment_count)
#define PLACE_COUNT 4

static int is_same_path(const char *left, const char *right) {
    return RUNTIME_HOST_IS_WINDOWS ? runtime_equal_ignoring_ascii_case(left, right) : strcmp(left, right) == 0;
}

/** Whether the place at `index` lies inside another, or repeats one listed before it. */
static int is_covered(char *const *resolved, size_t index) {
    for (size_t other = 0; other < PLACE_COUNT; other++) {
        if (other == index) {
            continue;
        }
        if (runtime_cache_is_strictly_inside(resolved[other], resolved[index]) ||
            (other < index && is_same_path(resolved[other], resolved[index]))) {
            return 1;
        }
    }
    return 0;
}

static int fill_paths(char *const *resolved, runtime_testing_real_places_answer *answer) {
    const char **paths = NULL;
    size_t count = 0;
    if (!runtime_storage_take_array(&answer->storage, PLACE_COUNT, sizeof *paths, (void **)&paths)) {
        return 0;
    }
    for (size_t index = 0; index < PLACE_COUNT; index++) {
        if (!is_covered(resolved, index) && !runtime_storage_copy(&answer->storage, resolved[index], &paths[count++])) {
            return 0;
        }
    }
    answer->paths = paths;
    answer->path_count = count;
    return 1;
}

static spisor_status answer_places(const runtime_places_answer *places, runtime_testing_real_places_answer *answer) {
    const char *const found[PLACE_COUNT] = {places->portable, places->state, places->cache, places->secrets_file};
    char *resolved[PLACE_COUNT] = {NULL, NULL, NULL, NULL};
    int complete = 1;
    for (size_t index = 0; index < PLACE_COUNT; index++) {
        resolved[index] = runtime_cache_resolve(found[index], NULL, 0);
        complete = complete && resolved[index] != NULL;
    }
    complete = complete && fill_paths(resolved, answer);
    for (size_t index = 0; index < PLACE_COUNT; index++) {
        spisor_memory_release(resolved[index]);
    }
    if (!complete) {
        runtime_release_stored_answer(answer, RUNTIME_TESTING_REAL_PLACES_ANSWER_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

spisor_status runtime_testing_real_places(void *context, const runtime_testing_real_places_argument *argument,
                                          runtime_testing_real_places_answer *answer, spisor_refusal *refusal) {
    const runtime_places_definition *places = (const runtime_places_definition *)runtime_places_definition_of();
    runtime_places_resolve_argument resolve = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer resolved = SPISOR_INIT(runtime_places_answer);
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, RUNTIME_TESTING_REAL_PLACES_ANSWER_SHAPE, answer->path_count)) {
        return spisor_refuse_shape(refusal, "runtime.testing's real_places");
    }
    resolve.environment = argument->environment;
    resolve.environment_count = argument->environment_count;
    status = places->resolve(places->context, &resolve, &resolved, refusal);
    if (status == SPISOR_OK) {
        status = answer_places(&resolved, answer);
        places->release_answer(places->context, &resolved);
    }
    return status;
}
