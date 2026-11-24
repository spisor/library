#ifndef RUNTIME_TESTING_OPERATIONS_H
#define RUNTIME_TESTING_OPERATIONS_H

#include "runtime_testing/home.h"

#define RUNTIME_TESTING_HOME_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_testing_home_answer, storage)
#define RUNTIME_TESTING_REAL_PLACES_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_testing_real_places_answer, storage)

/** runtime.testing's lay_out_home, as include/runtime_testing/home.h documents it. */
spisor_status runtime_testing_lay_out_home(void *context, const runtime_testing_home_argument *argument,
                                           runtime_testing_home_answer *answer, spisor_refusal *refusal);

/** runtime.testing's real_places, as include/runtime_testing/home.h documents it. */
spisor_status runtime_testing_real_places(void *context, const runtime_testing_real_places_argument *argument,
                                          runtime_testing_real_places_answer *answer, spisor_refusal *refusal);

#endif
