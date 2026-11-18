#include "host_holds.h"

#include "allocation.h"
#include "host_definition.h"
#include "host_output.h"
#include "host_service.h"
#include "runtime/processes.h"

static void release_one(const host_context *context, const char *host_id, double holder) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_holder_argument argument = SPISOR_INIT(runtime_processes_holder_argument);
    runtime_processes_release_answer answer = SPISOR_INIT(runtime_processes_release_answer);
    argument.state = context->state;
    argument.host_id = host_id;
    argument.holder = (int64_t)holder;
    processes->release(processes->context, &argument, &answer, NULL);
    processes->release_release_answer(processes->context, &answer);
}

static void release_before(const host_context *context, const cJSON *starts, const cJSON *last, double holder) {
    for (const cJSON *held = starts->child; held != NULL && held != last; held = held->next) {
        release_one(context, held->string, holder);
    }
}

/** Holds host_id, started as `start` says when nothing runs for it; 0 with *refusal set, NULL out of memory. */
static int hold_one(const host_context *context, const cJSON *start, double holder, int64_t *pid, cJSON **refusal) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_hold_argument argument = SPISOR_INIT(runtime_processes_hold_argument);
    runtime_processes_hold_answer answer = SPISOR_INIT(runtime_processes_hold_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(start, "environment");
    const cJSON *listed = cJSON_GetObjectItemCaseSensitive(start, "arguments");
    const char **arguments = host_texts_of(listed, &argument.argument_count);
    runtime_variable *variables =
        environment == NULL ? NULL : host_variables_of(environment, &argument.environment_count);
    int complete = arguments != NULL && (environment == NULL || variables != NULL);
    spisor_status status = complete ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    argument.state = context->state;
    argument.host_id = start->string;
    argument.holder = (int64_t)holder;
    argument.executable = host_text(start, "executable");
    argument.arguments = arguments;
    argument.environment = variables;
    argument.directory = host_text(start, "directory");
    if (status == SPISOR_OK) {
        status = processes->hold(processes->context, &argument, &answer, &declined);
        *refusal = status == SPISOR_OK ? NULL : host_refusal_json(&declined);
    }
    *pid = answer.pid;
    processes->release_hold_answer(processes->context, &answer);
    processes->release_refusal(processes->context, &declined);
    spisor_memory_release(variables);
    spisor_memory_release(arguments);
    return status == SPISOR_OK;
}

int host_hold_all(const host_context *context, const cJSON *starts, double holder, cJSON *pids, cJSON **refusal) {
    *refusal = NULL;
    for (const cJSON *start = starts->child; start != NULL; start = start->next) {
        int64_t pid = 0;
        if (!hold_one(context, start, holder, &pid, refusal)) {
            release_before(context, starts, start, holder);
            return 0;
        }
        if (pids != NULL && cJSON_AddNumberToObject(pids, start->string, (double)pid) == NULL) {
            release_before(context, starts, start->next, holder);
            return 0;
        }
    }
    return 1;
}

void host_release_all(const host_context *context, const cJSON *starts, double holder) {
    release_before(context, starts, NULL, holder);
}
