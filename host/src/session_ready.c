#include "session_ready.h"

#include <string.h>

#include "host_service.h"
#include "paths.h"
#include "processes_ready.h"
#include "ready_timeout.h"

static cJSON *await_argument(const host_context *context, const char *host_id, double pid) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && (cJSON_AddStringToObject(argument, "state", context->state) == NULL ||
                             cJSON_AddStringToObject(argument, "hostId", host_id) == NULL ||
                             cJSON_AddNumberToObject(argument, "pid", pid) == NULL ||
                             cJSON_AddNumberToObject(argument, "timeoutMilliseconds", host_ready_timeout_milliseconds()) == NULL)) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

cJSON *host_await_session_environment(const host_context *context, const char *host_id, double pid, cJSON **refusal) {
    cJSON *answer = host_call(runtime_processes_await_ready, await_argument(context, host_id, pid), refusal);
    cJSON *environment = answer == NULL ? NULL : cJSON_DetachItemFromObjectCaseSensitive(answer, "environment");
    cJSON_Delete(answer);
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
