#include "runtime/commands.h"

#include "answer_storage.h"
#include "commands.h"
#include "definition_support.h"

#define COMMAND_SHAPE RUNTIME_SHAPE_THROUGH(runtime_command_argument, environment_count)
#define RENAME_SHAPE RUNTIME_SHAPE_THROUGH(runtime_commands_rename_argument, environment_count)
#define DELETE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_commands_delete_argument, name)
#define ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_command_answer, storage)

static spisor_status add_arguments(cJSON *call, const char *const *arguments, size_t count,
                                   spisor_refusal *refusal) {
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

/** {environment, arguments?}, the JSON a call reads beside its texts; NULL with the status when it is not built. */
static spisor_status call_of(const runtime_variable *environment, size_t environment_count,
                             const char *const *arguments, size_t argument_count, int takes_arguments, cJSON **call,
                             spisor_refusal *refusal) {
    cJSON *json = NULL;
    spisor_status status = runtime_environment_json(environment, environment_count, &json);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    *call = status == SPISOR_OK ? cJSON_CreateObject() : NULL;
    if (*call == NULL || !cJSON_AddItemToObject(*call, "environment", json)) {
        cJSON_Delete(json);
        cJSON_Delete(*call);
        *call = NULL;
        return SPISOR_OUT_OF_MEMORY;
    }
    status = takes_arguments ? add_arguments(*call, arguments, argument_count, refusal) : SPISOR_OK;
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
static spisor_status serve_named(runtime_command_operation operation, runtime_command_request *commanded, cJSON *call,
                                 runtime_command_answer *answer, spisor_refusal *refusal) {
    char *name = runtime_storage_take(&answer->storage, RUNTIME_COMMAND_NAME_SIZE);
    spisor_status status;
    commanded->environment = cJSON_GetObjectItemCaseSensitive(call, "environment");
    commanded->arguments = cJSON_GetObjectItemCaseSensitive(call, "arguments");
    status = name == NULL ? SPISOR_OUT_OF_MEMORY : runtime_commands_serve(operation, commanded, name, refusal);
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
    runtime_command_request commanded;
    cJSON *call = NULL;
    spisor_status status;
    if (argument == NULL || !runtime_has_shape(argument->size, COMMAND_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, operation == RUNTIME_COMMAND_ADD ? "commands' add" : "commands' request");
    }
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    status = call_of(argument->environment, argument->environment_count, argument->arguments,
                     argument->argument_count, 1, &call, refusal);
    commanded = (runtime_command_request){argument->state, argument->host, NULL, argument->home, argument->name,
                                        argument->target, NULL, NULL, NULL};
    return status == SPISOR_OK ? serve_named(operation, &commanded, call, answer, refusal) : status;
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
    runtime_command_request commanded;
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
    status = call_of(argument->environment, argument->environment_count, NULL, 0, 0, &call, refusal);
    commanded = (runtime_command_request){argument->state, argument->host, NULL, NULL, NULL,
                                        NULL, NULL, argument->from, argument->to};
    return status == SPISOR_OK ? serve_named(RUNTIME_COMMAND_RENAME, &commanded, call, answer, refusal) : status;
}

static spisor_status remove_command(void *context, const runtime_commands_delete_argument *argument,
                                    spisor_refusal *refusal) {
    char name[RUNTIME_COMMAND_NAME_SIZE];
    runtime_command_request commanded;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, DELETE_SHAPE)) {
        return runtime_refuse_shape(refusal, "commands' remove");
    }
    commanded = (runtime_command_request){argument->state, argument->host, NULL, NULL, argument->name,
                                        NULL, NULL, NULL, NULL};
    return runtime_commands_serve(RUNTIME_COMMAND_DELETE, &commanded, name, refusal);
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
