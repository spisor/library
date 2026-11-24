#include "answer_storage.h"
#include "operations.h"
#include "spisor/definition.h"

static void release_home_answer(void *context, runtime_testing_home_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RUNTIME_TESTING_HOME_ANSWER_SHAPE);
}

static void release_real_places_answer(void *context, runtime_testing_real_places_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RUNTIME_TESTING_REAL_PLACES_ANSWER_SHAPE);
}

static const runtime_testing_definition definition = {
    sizeof(runtime_testing_definition), NULL,           runtime_testing_lay_out_home, runtime_testing_real_places,
    release_home_answer,                release_real_places_answer, spisor_release_refusal,
};

RUNTIME_TESTING_API const void *runtime_testing_definition_of(void) {
    return &definition;
}
