#include "runtime/processes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "host_references.h"
#include "json_service.h"
#include "paths.h"
#include "processes.h"
#include "processes_ready.h"
#include "run_records.h"
#include "spisor/path.h"

#define HEADER "the processes header"
#define HOLD_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_hold_argument, directory)
#define HOLD_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_hold_answer, storage)
#define HOLDER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_holder_argument, holder)
#define RELEASE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_release_answer, storage)
#define PRUNE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_prune_argument, check_milliseconds)
#define PRUNE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_prune_answer, storage)
#define READY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_ready_argument, fix)
#define AWAIT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_await_argument, timeout_milliseconds)
#define AWAIT_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_await_answer, storage)
#define RECORD_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_record_argument, record)
#define RECORD_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_record_answer, storage)
#define LIVE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_live_argument, now)
#define LIVE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_live_answer, storage)
#define REMOVE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_processes_remove_argument, record)

/** The state and host a call is for, the host NULL for a call that takes none. */
typedef struct process_target {
    const char *state;
    const char *host_id;
} process_target;

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static int is_time(double value) {
    return isfinite(value) && value >= 0;
}

static spisor_status check_target(const process_target *target, int takes_host, spisor_refusal *refusal) {
    if (!runtime_is_place(target->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    return takes_host ? runtime_check_host_id(refusal, target->host_id, HEADER) : SPISOR_OK;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static int add_number(cJSON *object, const char *name, double number) {
    return cJSON_AddNumberToObject(object, name, number) != NULL;
}

/** {state, hostId?}; NULL when memory runs out. */
static cJSON *target_call(const process_target *target) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !add_text(call, "state", target->state) || !add_text(call, "hostId", target->host_id)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Deletes `call` and answers NULL unless `built`. */
static cJSON *built_call(cJSON *call, int built) {
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int add_environment(cJSON *object, const char *name, const runtime_variable *environment, size_t count) {
    cJSON *json = NULL;
    spisor_status status = runtime_environment_json(environment, count, &json);
    if (status != SPISOR_OK || !cJSON_AddItemToObject(object, name, json)) {
        cJSON_Delete(json);
        return 0;
    }
    return 1;
}

static int64_t number_of(const cJSON *answered, const char *name) {
    return (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(answered, name));
}

static uint32_t flag_of(const cJSON *answered, const char *name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, name)) ? 1 : 0;
}

/**
 * Serves an operation whose answer holds no text, taking the answer's storage before the operation writes anything,
 * so filling it cannot run out of memory after the operation acted: `fill` sets fields only.
 */
static spisor_status serve_fixed(runtime_json_operation operation, cJSON *call, void (*fill)(const cJSON *, void *),
                                 void *answer, void **storage, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = NULL;
    if (runtime_start_storage(storage) != SPISOR_OK) {
        cJSON_Delete(call);
        return SPISOR_OUT_OF_MEMORY;
    }
    answered = runtime_call_operation(operation, call, &operation_refusal);
    if (answered == NULL) {
        runtime_release_storage(*storage);
        *storage = NULL;
        return runtime_refuse_json(refusal, operation_refusal);
    }
    fill(answered, answer);
    cJSON_Delete(answered);
    return SPISOR_OK;
}

static void fill_hold(const cJSON *answered, void *filled) {
    runtime_processes_hold_answer *answer = filled;
    answer->pid = number_of(answered, "pid");
    answer->started = flag_of(answered, "started");
    answer->outside_job = flag_of(answered, "outsideJob");
}

static spisor_status check_hold(const runtime_processes_hold_argument *argument, spisor_refusal *refusal) {
    process_target target = {argument->state, argument->host_id};
    spisor_status status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && argument->holder <= 0) {
        status = runtime_refuse_member(refusal, "holder", HEADER);
    }
    if (status == SPISOR_OK && (!runtime_is_text(argument->executable) ||
                                !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, argument->executable))) {
        status = runtime_refuse_member(refusal, "executable", HEADER);
    }
    if (status == SPISOR_OK && !runtime_are_arguments(argument->arguments, argument->argument_count)) {
        status = runtime_refuse_member(refusal, "arguments", HEADER);
    }
    if (status == SPISOR_OK && argument->directory != NULL && !runtime_is_text(argument->directory)) {
        status = runtime_refuse_member(refusal, "directory", HEADER);
    }
    if (status == SPISOR_OK && !runtime_is_environment(argument->environment, argument->environment_count)) {
        status = runtime_refuse_environment(refusal);
    }
    return status;
}

/** The start member, with an environment only for a caller that gave one, so NULL leaves the caller's to inherit. */
static cJSON *hold_start(const runtime_processes_hold_argument *argument) {
    cJSON *start = cJSON_CreateObject();
    cJSON *arguments = start == NULL ? NULL : cJSON_AddArrayToObject(start, "arguments");
    int built = arguments != NULL && add_text(start, "executable", argument->executable) &&
                add_text(start, "directory", argument->directory) &&
                (argument->environment == NULL ||
                 add_environment(start, "environment", argument->environment, argument->environment_count));
    for (size_t index = 0; built && index < argument->argument_count; index++) {
        cJSON *text = cJSON_CreateString(argument->arguments[index]);
        built = text != NULL && cJSON_AddItemToArray(arguments, text);
        if (!built) {
            cJSON_Delete(text);
        }
    }
    return built_call(start, built);
}

static cJSON *hold_call(const runtime_processes_hold_argument *argument) {
    process_target target = {argument->state, argument->host_id};
    cJSON *call = target_call(&target);
    cJSON *start = call == NULL ? NULL : hold_start(argument);
    int built = start != NULL && cJSON_AddItemToObject(call, "start", start);
    if (start != NULL && !built) {
        cJSON_Delete(start);
    }
    return built_call(call, built && add_number(call, "holder", (double)argument->holder));
}

static spisor_status hold(void *context, const runtime_processes_hold_argument *argument,
                          runtime_processes_hold_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, HOLD_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, HOLD_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' hold");
    }
    status = check_hold(argument, refusal);
    return status != SPISOR_OK ? status
                               : serve_fixed(runtime_processes_hold_operation, hold_call(argument), fill_hold,
                                             answer, &answer->storage, refusal);
}

static spisor_status fill_release(const cJSON *answered, void *filled) {
    runtime_processes_release_answer *answer = filled;
    const cJSON *holders = cJSON_GetObjectItemCaseSensitive(answered, "holders");
    const cJSON *holder;
    int64_t *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(holders),
                                                           sizeof *copies, (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->holders = copies;
    cJSON_ArrayForEach(holder, holders) {
        copies[answer->holder_count++] = (int64_t)cJSON_GetNumberValue(holder);
    }
    return SPISOR_OK;
}

static spisor_status release(void *context, const runtime_processes_holder_argument *argument,
                             runtime_processes_release_answer *answer, spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, HOLDER_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RELEASE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' release");
    }
    target = (process_target){argument->state, argument->host_id};
    status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && argument->holder <= 0) {
        status = runtime_refuse_member(refusal, "holder", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = target_call(&target);
    call = built_call(call, call != NULL && add_number(call, "holder", (double)argument->holder));
    return runtime_serve_operation(runtime_processes_release_operation, call, fill_release, answer,
                                   RELEASE_ANSWER_SHAPE, refusal);
}

static void fill_prune(const cJSON *answered, void *filled) {
    runtime_processes_prune_answer *answer = filled;
    answer->exit = flag_of(answered, "exit");
}

static cJSON *prune_call(const runtime_processes_prune_argument *argument) {
    process_target target = {argument->state, argument->host_id};
    cJSON *call = target_call(&target);
    int built = call != NULL && add_number(call, "self", (double)argument->self) &&
                (argument->now == 0 || add_number(call, "now", argument->now)) &&
                (argument->check_milliseconds == 0 ||
                 add_number(call, "checkMilliseconds", argument->check_milliseconds));
    return built_call(call, built);
}

static spisor_status prune(void *context, const runtime_processes_prune_argument *argument,
                           runtime_processes_prune_answer *answer, spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PRUNE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, PRUNE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' prune");
    }
    target = (process_target){argument->state, argument->host_id};
    status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && argument->self <= 0) {
        status = runtime_refuse_member(refusal, "self", HEADER);
    }
    if (status == SPISOR_OK && (!is_time(argument->now) || !is_time(argument->check_milliseconds))) {
        status = runtime_refuse_member(refusal, !is_time(argument->now) ? "now" : "check_milliseconds", HEADER);
    }
    return status != SPISOR_OK ? status
                               : serve_fixed(runtime_processes_prune_operation, prune_call(argument), fill_prune,
                                             answer, &answer->storage, refusal);
}

/** Serves `call` through an operation that answers {}, with no answer to fill. */
static spisor_status serve_unanswered(runtime_json_operation operation, cJSON *call, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_call_operation(operation, call, &operation_refusal);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal) : SPISOR_OK;
    cJSON_Delete(answered);
    return status;
}

static spisor_status check_ready(const runtime_processes_ready_argument *argument, spisor_refusal *refusal) {
    process_target target = {argument->state, argument->host_id};
    spisor_status status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && argument->pid <= 0) {
        status = runtime_refuse_member(refusal, "pid", HEADER);
    }
    if (status == SPISOR_OK && (argument->refused != NULL || argument->fix != NULL) &&
        (!runtime_is_text(argument->refused) || !runtime_is_text(argument->fix) || argument->environment != NULL)) {
        status = runtime_refuse_member(
            refusal, argument->environment != NULL ? "environment beside refused" : "refused and fix", HEADER);
    }
    if (status == SPISOR_OK && !runtime_is_environment(argument->environment, argument->environment_count)) {
        status = runtime_refuse_environment(refusal);
    }
    return status;
}

static cJSON *ready_call(const runtime_processes_ready_argument *argument) {
    process_target target = {argument->state, argument->host_id};
    cJSON *call = target_call(&target);
    int built = call != NULL && add_number(call, "pid", (double)argument->pid) &&
                add_text(call, "refused", argument->refused) && add_text(call, "fix", argument->fix) &&
                (argument->refused != NULL ||
                 add_environment(call, "environment", argument->environment, argument->environment_count));
    return built_call(call, built);
}

static spisor_status ready(void *context, const runtime_processes_ready_argument *argument,
                           spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, READY_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' ready");
    }
    status = check_ready(argument, refusal);
    return status != SPISOR_OK ? status
                               : serve_unanswered(runtime_processes_ready_operation, ready_call(argument), refusal);
}

static spisor_status fill_await(const cJSON *answered, void *filled) {
    runtime_processes_await_answer *answer = filled;
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(answered, "environment");
    const cJSON *member;
    runtime_variable *variables;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(environment),
                                                           sizeof *variables, (void **)&variables)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->environment = variables;
    cJSON_ArrayForEach(member, environment) {
        runtime_variable *each = &variables[answer->environment_count++];
        if (!runtime_storage_copy(&answer->storage, member->string, &each->name) ||
            !runtime_storage_copy(&answer->storage, cJSON_GetStringValue(member), &each->value)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status await_ready(void *context, const runtime_processes_await_argument *argument,
                                 runtime_processes_await_answer *answer, spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, AWAIT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, AWAIT_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' await_ready");
    }
    target = (process_target){argument->state, argument->host_id};
    status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && (argument->pid <= 0 || argument->timeout_milliseconds <= 0)) {
        status = runtime_refuse_member(refusal, argument->pid <= 0 ? "pid" : "timeout_milliseconds", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = target_call(&target);
    call = built_call(call, call != NULL && add_number(call, "pid", (double)argument->pid) &&
                                add_number(call, "timeoutMilliseconds", (double)argument->timeout_milliseconds));
    return runtime_serve_operation(runtime_processes_await_ready_operation, call, fill_await, answer,
                                   AWAIT_ANSWER_SHAPE, refusal);
}

static spisor_status fill_record(const cJSON *answered, void *filled) {
    runtime_processes_record_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status == SPISOR_OK && !runtime_storage_copy(&answer->storage, text_of(answered, "record"), &answer->record)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static cJSON *record_call(const runtime_processes_record_argument *argument) {
    process_target target = {argument->state, argument->host_id};
    cJSON *call = target_call(&target);
    cJSON *run = call == NULL ? NULL : cJSON_AddObjectToObject(call, "run");
    int built = run != NULL && add_text(call, "record", argument->record) &&
                (argument->check_milliseconds == 0 ||
                 add_number(call, "checkMilliseconds", argument->check_milliseconds));
    for (size_t index = 0; built && index < argument->run_count; index++) {
        built = add_text(run, argument->run[index].id, argument->run[index].version);
    }
    return built_call(call, built);
}

/** Records the run, and when the answer cannot be filled removes a record this call wrote anew. */
static spisor_status serve_record(const runtime_processes_record_argument *argument, cJSON *call,
                                  runtime_processes_record_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_call_operation(runtime_processes_record_run_operation, call, &operation_refusal);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                            : fill_record(answered, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, RECORD_ANSWER_SHAPE);
        if (argument->record == NULL) {
            runtime_remove_run_record(argument->state, argument->host_id, text_of(answered, "record"));
        }
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status record_run(void *context, const runtime_processes_record_argument *argument,
                                runtime_processes_record_answer *answer, spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RECORD_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RECORD_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' record_run");
    }
    target = (process_target){argument->state, argument->host_id};
    status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && !runtime_are_item_versions(argument->run, argument->run_count)) {
        status = runtime_refuse_member(refusal, "run", HEADER);
    }
    if (status == SPISOR_OK && !is_time(argument->check_milliseconds)) {
        status = runtime_refuse_member(refusal, "check_milliseconds", HEADER);
    }
    if (status == SPISOR_OK && argument->record != NULL && !runtime_is_text(argument->record)) {
        status = runtime_refuse_member(refusal, "record", HEADER);
    }
    return status != SPISOR_OK ? status : serve_record(argument, record_call(argument), answer, refusal);
}

static spisor_status fill_run(void **storage, const cJSON *object, runtime_processes_run *run) {
    runtime_item_version *versions;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *versions,
                                    (void **)&versions)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    run->versions = versions;
    cJSON_ArrayForEach(member, object) {
        runtime_item_version *each = &versions[run->version_count++];
        if (!runtime_storage_copy(storage, member->string, &each->id) ||
            !runtime_storage_copy(storage, cJSON_GetStringValue(member), &each->version)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_live(const cJSON *answered, void *filled) {
    runtime_processes_live_answer *answer = filled;
    const cJSON *runs = cJSON_GetObjectItemCaseSensitive(answered, "runs");
    const cJSON *entry;
    runtime_processes_run *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(runs),
                                                           sizeof *copies, (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->runs = copies;
    cJSON_ArrayForEach(entry, runs) {
        if (fill_run(&answer->storage, entry, &copies[answer->run_count++]) != SPISOR_OK) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status live_runs(void *context, const runtime_processes_live_argument *argument,
                               runtime_processes_live_answer *answer, spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, LIVE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, LIVE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' live_runs");
    }
    target = (process_target){argument->state, NULL};
    status = check_target(&target, 0, refusal);
    if (status == SPISOR_OK && !is_time(argument->now)) {
        status = runtime_refuse_member(refusal, "now", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = target_call(&target);
    call = built_call(call, call != NULL && (argument->now == 0 || add_number(call, "now", argument->now)));
    return runtime_serve_operation(runtime_processes_live_runs_operation, call, fill_live, answer, LIVE_ANSWER_SHAPE,
                                   refusal);
}

static spisor_status remove_runs(void *context, const runtime_processes_remove_argument *argument,
                                 spisor_refusal *refusal) {
    process_target target;
    spisor_status status;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, REMOVE_SHAPE)) {
        return runtime_refuse_shape(refusal, "processes' remove_runs");
    }
    target = (process_target){argument->state, argument->host_id};
    status = check_target(&target, 1, refusal);
    if (status == SPISOR_OK && argument->record != NULL && !runtime_is_text(argument->record)) {
        status = runtime_refuse_member(refusal, "record", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = target_call(&target);
    call = built_call(call, call != NULL && add_text(call, "record", argument->record));
    return serve_unanswered(runtime_processes_remove_runs_operation, call, refusal);
}

static void release_hold_answer(void *context, runtime_processes_hold_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, HOLD_ANSWER_SHAPE);
}

static void release_release_answer(void *context, runtime_processes_release_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RELEASE_ANSWER_SHAPE);
}

static void release_prune_answer(void *context, runtime_processes_prune_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PRUNE_ANSWER_SHAPE);
}

static void release_await_answer(void *context, runtime_processes_await_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, AWAIT_ANSWER_SHAPE);
}

static void release_record_answer(void *context, runtime_processes_record_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RECORD_ANSWER_SHAPE);
}

static void release_live_answer(void *context, runtime_processes_live_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LIVE_ANSWER_SHAPE);
}

static const runtime_processes_definition definition = {
    sizeof(runtime_processes_definition),
    NULL,
    hold,
    release,
    prune,
    ready,
    await_ready,
    record_run,
    live_runs,
    remove_runs,
    release_hold_answer,
    release_release_answer,
    release_prune_answer,
    release_await_answer,
    release_record_answer,
    release_live_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_processes_definition_of(void) {
    return &definition;
}
