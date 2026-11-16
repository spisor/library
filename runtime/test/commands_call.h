#ifndef RUNTIME_TEST_COMMANDS_CALL_H
#define RUNTIME_TEST_COMMANDS_CALL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "runtime/commands.h"
#include "spisor/status.h"

/**
 * @file
 * Calls the commands definition with a request a test writes as JSON, {places: {state}, host, environment, home,
 * name, target, arguments, from, to}, and answers what it answered as JSON a test reads, {name?} or {deleted: true}.
 * An environment's text members become its variables, so an environment without a text PATH arrives without PATH.
 * Test code only: the JSON is the test's data, never the boundary.
 */

typedef enum commands_operation {
    COMMANDS_REQUEST,
    COMMANDS_RENAME,
    COMMANDS_DELETE,
    COMMANDS_ADD
} commands_operation;

/** What a call answered: its status, the answer as JSON (NULL unless OK), and the refusal's texts. */
typedef struct commands_result {
    spisor_status status;
    cJSON *tree;
    char detail[1024];
    char fix[1024];
} commands_result;

/** The variables and arguments read from a request, each borrowing the request's texts. */
typedef struct commands_parts {
    runtime_variable *environment;
    size_t environment_count;
    const char **arguments;
    size_t argument_count;
} commands_parts;

static inline const char *commands_member_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static inline void commands_parts_of(const cJSON *request, commands_parts *parts) {
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(request, "environment");
    const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(request, "arguments");
    const cJSON *variables = cJSON_IsObject(environment) ? environment : NULL;
    const cJSON *item;
    memset(parts, 0, sizeof *parts);
    parts->environment = calloc((size_t)cJSON_GetArraySize(environment) + 1, sizeof *parts->environment);
    parts->arguments = calloc((size_t)cJSON_GetArraySize(arguments) + 1, sizeof *parts->arguments);
    cJSON_ArrayForEach(item, variables) {
        if (cJSON_IsString(item)) {
            parts->environment[parts->environment_count].name = item->string;
            parts->environment[parts->environment_count++].value = item->valuestring;
        }
    }
    cJSON_ArrayForEach(item, arguments) {
        parts->arguments[parts->argument_count++] = cJSON_GetStringValue(item);
    }
}

static inline void commands_release_parts(commands_parts *parts) {
    free(parts->environment);
    free((void *)parts->arguments);
}

static inline void commands_keep(commands_result *result, const runtime_command_answer *answer,
                                 const spisor_refusal *refusal) {
    if (result->status == SPISOR_OK) {
        result->tree = cJSON_CreateObject();
        if (answer != NULL && answer->name != NULL) {
            cJSON_AddStringToObject(result->tree, "name", answer->name);
        }
        if (answer == NULL) {
            cJSON_AddTrueToObject(result->tree, "deleted");
        }
    }
    snprintf(result->detail, sizeof result->detail, "%s", refusal->detail == NULL ? "" : refusal->detail);
    snprintf(result->fix, sizeof result->fix, "%s", refusal->fix == NULL ? "" : refusal->fix);
}

/** Calls one commands operation with the request a test wrote, deleting the request. */
static inline commands_result commands_call(commands_operation operation, cJSON *request) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    const char *state = commands_member_text(cJSON_GetObjectItemCaseSensitive(request, "places"), "state");
    const char *host = commands_member_text(request, "host");
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    commands_parts parts;
    commands_result result;
    memset(&result, 0, sizeof result);
    commands_parts_of(request, &parts);
    if (operation == COMMANDS_DELETE) {
        runtime_commands_delete_argument argument = SPISOR_INIT(runtime_commands_delete_argument);
        argument.state = state;
        argument.host = host;
        argument.name = commands_member_text(request, "name");
        result.status = commands->remove(commands->context, &argument, &refusal);
        commands_keep(&result, NULL, &refusal);
    } else if (operation == COMMANDS_RENAME) {
        runtime_commands_rename_argument argument = SPISOR_INIT(runtime_commands_rename_argument);
        argument.state = state;
        argument.host = host;
        argument.from = commands_member_text(request, "from");
        argument.to = commands_member_text(request, "to");
        argument.environment = parts.environment;
        argument.environment_count = parts.environment_count;
        result.status = commands->rename(commands->context, &argument, &answer, &refusal);
        commands_keep(&result, &answer, &refusal);
    } else {
        runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
        argument.state = state;
        argument.host = host;
        argument.home = commands_member_text(request, "home");
        argument.name = commands_member_text(request, "name");
        argument.target = commands_member_text(request, "target");
        argument.arguments = parts.arguments;
        argument.argument_count = parts.argument_count;
        argument.environment = parts.environment;
        argument.environment_count = parts.environment_count;
        result.status = operation == COMMANDS_ADD ? commands->add(commands->context, &argument, &answer, &refusal)
                                                  : commands->request(commands->context, &argument, &answer, &refusal);
        commands_keep(&result, &answer, &refusal);
    }
    commands->release_answer(commands->context, &answer);
    commands->release_refusal(commands->context, &refusal);
    commands_release_parts(&parts);
    cJSON_Delete(request);
    return result;
}

#endif
