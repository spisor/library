#include "runtime/standard.h"

#include <stdio.h>
#include <string.h>

#include <weivel/descriptions.h>
#include <weivel/host.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "host_run.h"
#include "process_environment.h"
#include "spisor/definition.h"
#include "standard_refusal.h"
#include "thread.h"

#define PLAN_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_host_plan, storage)
#define NEXT_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_host_next, storage)
#define NO_RUN_FIX "call host's start, and this operation only once it answered OK and before its stop"
#define SECOND_START "host's start was called again before its stop"

/** The one run a process makes, from host's start answering OK until its stop. */
static runtime_mutex run_lock = RUNTIME_MUTEX_INIT;
static runtime_host_run *current_run;

static spisor_status refuse_malformed(spisor_refusal *refusal, const char *detail, const char *fix) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static spisor_status refuse_without_run(spisor_refusal *refusal, const char *operation) {
    char detail[128];
    snprintf(detail, sizeof detail, "host's %s was called with no run started", operation);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, NO_RUN_FIX);
}

static int has_run(void) {
    int running;
    runtime_mutex_lock(&run_lock);
    running = current_run != NULL;
    runtime_mutex_unlock(&run_lock);
    return running;
}

static spisor_status fill_libraries(void **storage, const cJSON *libraries, weivel_standard_host_plan *plan) {
    weivel_standard_host_library *filled;
    const cJSON *library;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(libraries), sizeof *filled, (void **)&filled)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    plan->libraries = filled;
    cJSON_ArrayForEach(library, libraries) {
        weivel_standard_host_library *each = &filled[plan->library_count++];
        if (!runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(library, "binary")),
                                  &each->binary) ||
            !runtime_storage_copy(storage,
                                  cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(library, "declaration")),
                                  &each->declaration)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

/** The text `member` of `object`, or "" when it holds none, as a plugin whose manifest names no entry that is there. */
static const char *text_or_empty(const cJSON *object, const char *member) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
    return text == NULL ? "" : text;
}

static spisor_status fill_plugins(void **storage, const cJSON *located, weivel_standard_host_plan *plan) {
    weivel_standard_host_plugin *filled;
    const cJSON *plugin;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(located), sizeof *filled, (void **)&filled)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    plan->plugins = filled;
    cJSON_ArrayForEach(plugin, located) {
        weivel_standard_host_plugin *each = &filled[plan->plugin_count++];
        if (!runtime_storage_copy(storage, text_or_empty(plugin, "directory"), &each->directory) ||
            !runtime_storage_copy(storage, text_or_empty(plugin, "manifest"), &each->manifest) ||
            !runtime_storage_copy(storage, text_or_empty(plugin, "entry"), &each->entry)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_action(void **storage, const cJSON *action, weivel_standard_host_plan *plan) {
    const spisor_value *arguments = NULL;
    spisor_status status;
    if (action == NULL) {
        return SPISOR_OK;
    }
    if (!runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(action, "plugin")),
                              &plan->action_plugin) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(action, "name")),
                              &plan->action_name)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(action, "arguments"), &arguments);
    if (status == SPISOR_OK) {
        memcpy(&plan->action_arguments, arguments, sizeof plan->action_arguments);
    }
    return status;
}

/**
 * Fills an empty plan with what `run` plans.
 *
 * @implNote spisor_value is weivel_standard_value field for field (restatement_test), so the merged arguments are
 * copied into the plan as they are.
 */
static spisor_status fill_plan(const runtime_host_run *run, weivel_standard_host_plan *plan) {
    void *storage = NULL;
    spisor_status status = runtime_start_storage(&storage);
    if (status == SPISOR_OK) {
        status = fill_libraries(&storage, run->libraries, plan);
    }
    if (status == SPISOR_OK) {
        status = fill_plugins(&storage, cJSON_GetObjectItemCaseSensitive(run->located, "located"), plan);
    }
    if (status == SPISOR_OK) {
        status = fill_action(&storage, run->action, plan);
    }
    plan->storage = storage;
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(plan, PLAN_SHAPE);
        return status;
    }
    plan->mode = run->mode;
    runtime_zero_beyond(plan, sizeof *plan);
    return SPISOR_OK;
}

/** Starts the run the arguments name and fills the plan with it, holding the run until its stop. */
static spisor_status start_run(const char *const *arguments, size_t count, weivel_standard_host_plan *plan,
                               spisor_refusal *refusal) {
    cJSON *json_refusal = NULL;
    cJSON *environment = runtime_process_environment();
    runtime_host_run *run =
        environment == NULL ? NULL : runtime_host_run_start(arguments, count, environment, &json_refusal);
    spisor_status status = run == NULL ? runtime_refuse_json(refusal, json_refusal) : fill_plan(run, plan);
    cJSON_Delete(environment);
    if (status == SPISOR_OK) {
        current_run = run;
    } else {
        runtime_host_run_end(run);
    }
    return status;
}

static weivel_status start(void *context, const char *const *arguments, size_t argument_count,
                           weivel_standard_host_plan *plan, weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    (void)context;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!SPISOR_IS_EMPTY_COUNTED_ANSWER(plan, PLAN_SHAPE,
                                        plan->library_count | plan->plugin_count | plan->action_arguments.count)) {
        return runtime_answer_service(spisor_refuse_shape(&filled, "host's start"), &filled, refusal);
    }
    if (!runtime_are_arguments(arguments, argument_count)) {
        status = refuse_malformed(&filled, "host's start was called with an argument that is NULL",
                                  "pass the program's arguments, each a text, and NULL only for none");
        return runtime_answer_service(status, &filled, refusal);
    }
    runtime_mutex_lock(&run_lock);
    status = current_run != NULL ? spisor_refuse_as(&filled, SPISOR_REFUSED, SECOND_START, "call stop first")
                                 : start_run(arguments, argument_count, plan, &filled);
    runtime_mutex_unlock(&run_lock);
    return runtime_answer_service(status, &filled, refusal);
}

static weivel_status started(void *context, const weivel_standard_host_outcome *outcomes, size_t outcome_count,
                             weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status = SPISOR_OK;
    (void)context;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (outcomes == NULL && outcome_count != 0) {
        status = refuse_malformed(&filled, "host's started was called with no outcomes beside a count",
                                  "pass one outcome per plugin of the plan, and NULL only for none");
    } else if (!has_run()) {
        status = refuse_without_run(&filled, "started");
    }
    return runtime_answer_service(status, &filled, refusal);
}

static weivel_status tick(void *context, int64_t now, weivel_standard_host_next *next,
                          weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status = SPISOR_OK;
    (void)context;
    (void)now;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (!SPISOR_IS_EMPTY_ANSWER(next, NEXT_SHAPE)) {
        status = spisor_refuse_shape(&filled, "host's tick");
    } else if (!has_run()) {
        status = refuse_without_run(&filled, "tick");
    } else {
        SPISOR_CLEAR_ANSWER(next, NEXT_SHAPE);
        runtime_zero_beyond(next, sizeof *next);
    }
    return runtime_answer_service(status, &filled, refusal);
}

static weivel_status finished(void *context, const weivel_standard_value *result, const char *failure,
                              weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status = SPISOR_OK;
    (void)context;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if ((result == NULL) == (failure == NULL)) {
        status = refuse_malformed(&filled, "host's finished was called with both or neither of a result and a failure",
                                  "pass the action's result, or why it threw, and NULL for the other");
    } else if (!has_run()) {
        status = refuse_without_run(&filled, "finished");
    }
    return runtime_answer_service(status, &filled, refusal);
}

static weivel_status stop(void *context, uint32_t *exit_code, weivel_standard_service_refusal *refusal) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_status status = SPISOR_OK;
    runtime_host_run *run;
    (void)context;
    if (!runtime_is_usable_service_refusal(refusal)) {
        return WEIVEL_STATUS_MALFORMED;
    }
    if (exit_code == NULL) {
        status = refuse_malformed(&filled, "host's stop was called without an exit code to fill",
                                  "pass the exit code the program exits with");
        return runtime_answer_service(status, &filled, refusal);
    }
    runtime_mutex_lock(&run_lock);
    run = current_run;
    current_run = NULL;
    runtime_mutex_unlock(&run_lock);
    if (run == NULL) {
        status = refuse_without_run(&filled, "stop");
    } else {
        runtime_host_run_end(run);
        *exit_code = 0;
    }
    return runtime_answer_service(status, &filled, refusal);
}

static void release_plan(void *context, weivel_standard_host_plan *plan) {
    (void)context;
    runtime_release_stored_answer(plan, PLAN_SHAPE);
}

static void release_next(void *context, weivel_standard_host_next *next) {
    (void)context;
    if (next != NULL && spisor_has_shape(next->size, NEXT_SHAPE)) {
        SPISOR_CLEAR_ANSWER(next, NEXT_SHAPE);
    }
}

static const weivel_standard_host definition = {
    sizeof(weivel_standard_host), NULL, start, started, tick, finished, stop, release_plan, release_next,
    runtime_release_service_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_standard_host_definition_of(void) {
    return &definition;
}

RUNTIME_DEFINITION_API const weivel_standard_description *runtime_standard_host_definition_description(void) {
    return weivel_standard_host_description();
}
