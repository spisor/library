#include <string.h>

#include "answer_storage.h"
#include "holders.h"
#include "home_prepare.h"
#include "home_write.h"
#include "operations.h"
#include "spisor/allocation.h"
#include "spisor/definition.h"

#define ARGUMENT_SHAPE SPISOR_SHAPE_THROUGH(runtime_testing_home_argument, holder)

static int fill_variables(const runtime_testing_layout *layout, runtime_testing_home_answer *answer) {
    runtime_variable laid_out[RUNTIME_TESTING_VARIABLE_COUNT];
    runtime_variable *variables = NULL;
    runtime_testing_layout_variables(layout, laid_out);
    if (!runtime_storage_take_array(&answer->storage, RUNTIME_TESTING_VARIABLE_COUNT, sizeof *variables,
                                    (void **)&variables)) {
        return 0;
    }
    for (size_t index = 0; index < RUNTIME_TESTING_VARIABLE_COUNT; index++) {
        if (!runtime_storage_copy(&answer->storage, laid_out[index].name, &variables[index].name) ||
            !runtime_storage_copy(&answer->storage, laid_out[index].value, &variables[index].value)) {
            return 0;
        }
    }
    answer->variables = variables;
    answer->variable_count = RUNTIME_TESTING_VARIABLE_COUNT;
    return 1;
}

static int fill_installs(const runtime_testing_prepared_home *home, runtime_testing_home_answer *answer) {
    runtime_testing_placed *placed = NULL;
    if (!runtime_storage_take_array(&answer->storage, home->install_count, sizeof *placed, (void **)&placed)) {
        return 0;
    }
    for (size_t index = 0; index < home->install_count; index++) {
        const runtime_testing_prepared_install *install = &home->installs[index];
        if (!runtime_storage_copy(&answer->storage, install->id, &placed[index].id) ||
            !runtime_storage_copy(&answer->storage, install->bundle, &placed[index].bundle) ||
            !runtime_storage_copy(&answer->storage, install->settings_file, &placed[index].settings_file)) {
            return 0;
        }
    }
    answer->installs = placed;
    answer->install_count = home->install_count;
    return 1;
}

static int fill_answer(const runtime_testing_prepared_home *home, runtime_testing_home_answer *answer) {
    char *processes = runtime_processes_folder(home->layout.state);
    int filled = processes != NULL && fill_variables(&home->layout, answer) &&
                 runtime_storage_copy(&answer->storage, home->layout.application, &answer->home) &&
                 runtime_storage_copy(&answer->storage, processes, &answer->processes) && fill_installs(home, answer);
    spisor_memory_release(processes);
    return filled;
}

/**
 * Fills the answer first, so memory running out writes nothing, then writes, emptying the base again on failure.
 *
 * @implNote The emptying is best effort and its own status is not answered: the failure that caused it is what the
 * caller needs, and an emptying that itself fails, as it can once memory stays out, leaves the base as it stands.
 */
static spisor_status lay_out(const runtime_testing_prepared_home *home, runtime_testing_home_answer *answer,
                             spisor_refusal *refusal) {
    spisor_status status = fill_answer(home, answer) ? runtime_testing_write_home(home, refusal) : SPISOR_OUT_OF_MEMORY;
    if (status != SPISOR_OK) {
        (void)runtime_testing_empty_base(home->base);
        runtime_release_stored_answer(answer, RUNTIME_TESTING_HOME_ANSWER_SHAPE);
    }
    return status;
}

spisor_status runtime_testing_lay_out_home(void *context, const runtime_testing_home_argument *argument,
                                           runtime_testing_home_answer *answer, spisor_refusal *refusal) {
    runtime_testing_prepared_home home;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ARGUMENT_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, RUNTIME_TESTING_HOME_ANSWER_SHAPE,
                                        answer->variable_count | answer->install_count)) {
        return spisor_refuse_shape(refusal, "runtime.testing's lay_out_home");
    }
    status = runtime_testing_prepare_home(argument, &home, refusal);
    if (status == SPISOR_OK) {
        status = lay_out(&home, answer, refusal);
    }
    runtime_testing_free_prepared_home(&home);
    return status;
}
