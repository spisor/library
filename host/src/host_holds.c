#include "host_holds.h"

#include "processes.h"

static cJSON *holder_call(const host_context *context, const char *host_id, double holder) {
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "state", context->state) == NULL ||
                         cJSON_AddStringToObject(call, "hostId", host_id) == NULL ||
                         cJSON_AddNumberToObject(call, "holder", holder) == NULL)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static void release_one(const host_context *context, const char *host_id, double holder) {
    cJSON *refusal = NULL;
    cJSON_Delete(host_call(runtime_processes_release, holder_call(context, host_id, holder), &refusal));
    cJSON_Delete(refusal);
}

static void release_before(const host_context *context, const cJSON *starts, const cJSON *last, double holder) {
    for (const cJSON *held = starts->child; held != NULL && held != last; held = held->next) {
        release_one(context, held->string, holder);
    }
}

static int record_pid(cJSON *pids, const char *host_id, const cJSON *held) {
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(held, "pid");
    return pids == NULL || (cJSON_IsNumber(pid) && cJSON_AddNumberToObject(pids, host_id, pid->valuedouble) != NULL);
}

int host_hold_all(const host_context *context, const cJSON *starts, double holder, cJSON *pids, cJSON **refusal) {
    *refusal = NULL;
    for (const cJSON *start = starts->child; start != NULL; start = start->next) {
        cJSON *call = holder_call(context, start->string, holder);
        if (call != NULL && !cJSON_AddItemToObject(call, "start", cJSON_Duplicate(start, 1))) {
            cJSON_Delete(call);
            call = NULL;
        }
        cJSON *held = host_call(runtime_processes_hold, call, refusal);
        if (held == NULL || !record_pid(pids, start->string, held)) {
            cJSON_Delete(held);
            release_before(context, starts, start, holder);
            return 0;
        }
        cJSON_Delete(held);
    }
    return 1;
}

void host_release_all(const host_context *context, const cJSON *starts, double holder) {
    release_before(context, starts, NULL, holder);
}
