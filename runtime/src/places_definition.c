#include "runtime/places.h"

#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "json_service.h"
#include "paths.h"
#include "places.h"
#include "user_home.h"

#define ARGUMENT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_places_resolve_argument, environment_count)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_places_answer, storage)

static spisor_status fill_answer(const char *home, const runtime_places *places, runtime_places_answer *answer) {
    const char *texts[5] = {home, places->portable, places->state, places->cache, places->secrets_file};
    const char *packed[5];
    if (!runtime_pack_texts(texts, packed, 5, &answer->storage)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->home = packed[0];
    answer->portable = packed[1];
    answer->state = packed[2];
    answer->cache = packed[3];
    answer->secrets_file = packed[4];
    answer->force_file_store = places->force_file_store ? 1 : 0;
    return SPISOR_OK;
}

static spisor_status resolve_under(const char *home, const cJSON *environment, runtime_places_answer *answer,
                                   spisor_refusal *refusal) {
    runtime_places places;
    spisor_status status;
    int resolved = runtime_resolve_places(RUNTIME_HOST_IS_WINDOWS, RUNTIME_HOST_IS_MACOS, home, environment, &places);
    if (resolved == RUNTIME_PLACES_NO_LOCAL_APP_DATA) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, RUNTIME_NO_LOCAL_APP_DATA_MESSAGE,
                                 RUNTIME_NO_LOCAL_APP_DATA_FIX);
    }
    if (resolved != RUNTIME_PLACES_OK) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = fill_answer(home, &places, answer);
    runtime_free_places(&places);
    return status;
}

static spisor_status resolve_in(const runtime_places_resolve_argument *argument, const cJSON *environment,
                                runtime_places_answer *answer, spisor_refusal *refusal) {
    const char *home = argument->home != NULL ? argument->home : runtime_user_home_of(environment);
    if (home == NULL) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
    }
    if (!runtime_is_usable_home(home)) {
        return runtime_refuse_home(refusal);
    }
    return resolve_under(home, environment, answer, refusal);
}

static spisor_status resolve(void *context, const runtime_places_resolve_argument *argument,
                             runtime_places_answer *answer, spisor_refusal *refusal) {
    cJSON *environment = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "places' resolve");
    }
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    status = runtime_environment_json(argument->environment, argument->environment_count, &environment);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    if (status == SPISOR_OK) {
        status = resolve_in(argument, environment, answer, refusal);
    }
    cJSON_Delete(environment);
    return status;
}

static void release_answer(void *context, runtime_places_answer *answer) {
    (void)context;
    runtime_release_packed_answer(answer, ANSWER_SHAPE);
}

static const runtime_places_definition definition = {
    sizeof(runtime_places_definition), NULL, resolve, release_answer, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_places_definition_of(void) {
    return &definition;
}

int runtime_resolve_places_answer(const cJSON *environment, runtime_places_answer *answer, cJSON **refusal) {
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    runtime_variable *variables = runtime_variables_of_json(environment, &argument.environment_count);
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    argument.environment = variables;
    if (variables != NULL) {
        status = definition.resolve(definition.context, &argument, answer, &refused);
    }
    *refusal = status == SPISOR_OK ? NULL : runtime_refusal_of(&refused);
    runtime_release_refusal(NULL, &refused);
    free(variables);
    return status == SPISOR_OK;
}
