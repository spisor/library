#include "runtime/commands.h"

#include "answer_storage.h"
#include "commands.h"
#include "definition_support.h"

#define COMMAND_SHAPE RUNTIME_SHAPE_THROUGH(runtime_command_argument, environment_count)
#define RENAME_SHAPE RUNTIME_SHAPE_THROUGH(runtime_commands_rename_argument, environment_count)
#define DELETE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_commands_delete_argument, name)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_command_answer, storage)

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

/** {places: {state}, host}, the argument every operation's JSON begins with; NULL when memory runs out. */
static cJSON *call_of(const char *state, const char *host) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    if (places == NULL || !add_text(places, "state", state) || !add_text(call, "host", host)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status add_environment(cJSON *call, const runtime_variable *environment, size_t count,
                                     spisor_refusal *refusal) {
    cJSON *json = NULL;
    spisor_status status = runtime_environment_json(environment, count, &json);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    if (status == SPISOR_OK && !cJSON_AddItemToObject(call, "environment", json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static spisor_status add_arguments(cJSON *call, const char *const *arguments, size_t count, spisor_refusal *refusal) {
    cJSON *list;
    if (count > 0 && arguments == NULL) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries a count without its arguments",
                                 "send argument_count no larger than the arguments sent");
    }
    list = cJSON_AddArrayToObject(call, "arguments");
    for (size_t index = 0; list != NULL && index < count; index++) {
        cJSON *argument;
        if (arguments[index] == NULL) {
            return runtime_refuse_as(refusal, SPISOR_MALFORMED, "an argument of the command is NULL",
                                     "send each argument as text");
        }
        argument = cJSON_CreateString(arguments[index]);
        if (argument == NULL || !cJSON_AddItemToArray(list, argument)) {
            cJSON_Delete(argument);
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return list == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status command_call(const runtime_command_argument *argument, cJSON **call, spisor_refusal *refusal) {
    spisor_status status;
    *call = call_of(argument->state, argument->host);
    if (*call == NULL || !add_text(*call, "home", argument->home) || !add_text(*call, "name", argument->name) ||
        !add_text(*call, "target", argument->target)) {
        status = SPISOR_OUT_OF_MEMORY;
    } else {
        status = add_arguments(*call, argument->arguments, argument->argument_count, refusal);
    }
    if (status == SPISOR_OK) {
        status = add_environment(*call, argument->environment, argument->environment_count, refusal);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status rename_call(const runtime_commands_rename_argument *argument, cJSON **call,
                                 spisor_refusal *refusal) {
    spisor_status status;
    *call = call_of(argument->state, argument->host);
    status = *call != NULL && add_text(*call, "from", argument->from) && add_text(*call, "to", argument->to)
                 ? add_environment(*call, argument->environment, argument->environment_count, refusal)
                 : SPISOR_OUT_OF_MEMORY;
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

/**
 * Serves `call`, which it deletes, filling the answer with the name the operation answered.
 *
 * @implNote The name's block is taken before the operation runs, so once it has written nothing is left to allocate.
 */
static spisor_status serve_named(runtime_command_operation operation, cJSON *call, runtime_command_answer *answer,
                                 spisor_refusal *refusal) {
    char *name = runtime_storage_take(&answer->storage, RUNTIME_COMMAND_NAME_SIZE);
    spisor_status status = name == NULL ? SPISOR_OUT_OF_MEMORY : runtime_commands_serve(operation, call, name, refusal);
    cJSON_Delete(call);
    if (status == SPISOR_OK && name[0] != '\0') {
        answer->name = name;
    } else {
        runtime_release_stored_answer(answer, ANSWER_SHAPE);
    }
    return status;
}

static spisor_status give(runtime_command_operation operation, const runtime_command_argument *argument,
                          runtime_command_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    if (argument == NULL || !runtime_has_shape(argument->size, COMMAND_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, operation == RUNTIME_COMMAND_ADD ? "commands' add" : "commands' request");
    }
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    status = command_call(argument, &call, refusal);
    return status == SPISOR_OK ? serve_named(operation, call, answer, refusal) : status;
}

static spisor_status request(void *context, const runtime_command_argument *argument, runtime_command_answer *answer,
                             spisor_refusal *refusal) {
    (void)context;
    return give(RUNTIME_COMMAND_REQUEST, argument, answer, refusal);
}

static spisor_status add(void *context, const runtime_command_argument *argument, runtime_command_answer *answer,
                         spisor_refusal *refusal) {
    (void)context;
    return give(RUNTIME_COMMAND_ADD, argument, answer, refusal);
}

static spisor_status rename_command(void *context, const runtime_commands_rename_argument *argument,
                                    runtime_command_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RENAME_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "commands' rename");
    }
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    status = rename_call(argument, &call, refusal);
    return status == SPISOR_OK ? serve_named(RUNTIME_COMMAND_RENAME, call, answer, refusal) : status;
}

static spisor_status remove_command(void *context, const runtime_commands_delete_argument *argument,
                                    spisor_refusal *refusal) {
    char name[RUNTIME_COMMAND_NAME_SIZE];
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, DELETE_SHAPE)) {
        return runtime_refuse_shape(refusal, "commands' remove");
    }
    call = call_of(argument->state, argument->host);
    if (call == NULL || !add_text(call, "name", argument->name)) {
        cJSON_Delete(call);
        return SPISOR_OUT_OF_MEMORY;
    }
    status = runtime_commands_serve(RUNTIME_COMMAND_DELETE, call, name, refusal);
    cJSON_Delete(call);
    return status;
}

static void release_answer(void *context, runtime_command_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ANSWER_SHAPE);
}

static const runtime_commands_definition definition = {
    sizeof(runtime_commands_definition), NULL, request, rename_command, remove_command, add, release_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_commands_definition_of(void) {
    return &definition;
}
