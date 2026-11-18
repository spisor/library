#include "session_ready.h"

#include <string.h>

#include "host_definition.h"
#include "host_service.h"
#include "paths.h"
#include "processes_ready.h"
#include "ready_timeout.h"
#include "runtime/processes.h"

cJSON *host_await_session_environment(const host_context *context, const char *host_id, double pid, cJSON **refusal) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_await_argument argument = SPISOR_INIT(runtime_processes_await_argument);
    runtime_processes_await_answer answer = SPISOR_INIT(runtime_processes_await_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.state = context->state;
    argument.host_id = host_id;
    argument.pid = (int64_t)pid;
    argument.timeout_milliseconds = (int64_t)host_ready_timeout_milliseconds();
    spisor_status status = processes->await_ready(processes->context, &argument, &answer, &declined);
    cJSON *environment =
        status == SPISOR_OK ? host_variables_json(answer.environment, answer.environment_count) : NULL;
    *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    processes->release_await_answer(processes->context, &answer);
    processes->release_refusal(processes->context, &declined);
    return environment;
}

static int is_same_name(const char *left, const char *right) {
    return RUNTIME_HOST_IS_WINDOWS ? host_equals_ignoring_case(left, right) : strcmp(left, right) == 0;
}

static void remove_name(cJSON *environment, const char *name) {
    cJSON *variable = environment->child;
    while (variable != NULL) {
        cJSON *next = variable->next;
        if (is_same_name(variable->string, name)) {
            cJSON_DeleteItemFromObjectCaseSensitive(environment, variable->string);
        }
        variable = next;
    }
}

cJSON *host_overlay_environment(const cJSON *base, const cJSON *published) {
    cJSON *merged = cJSON_Duplicate(base, 1);
    for (const cJSON *variable = published == NULL ? NULL : published->child; merged != NULL && variable != NULL;
         variable = variable->next) {
        remove_name(merged, variable->string);
        if (!cJSON_IsString(variable) || !cJSON_AddItemToObject(merged, variable->string, cJSON_Duplicate(variable, 1))) {
            cJSON_Delete(merged);
            merged = NULL;
        }
    }
    return merged;
}
