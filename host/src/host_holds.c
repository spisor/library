#include "host_holds.h"

#include "host_output.h"
#include "host_service.h"
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

static const cJSON *pid_of(const cJSON *held) {
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(held, "pid");
    return cJSON_IsNumber(pid) ? pid : NULL;
}

static cJSON *unusable_pid_refusal(const char *host_id, const cJSON *held) {
    char *answer = cJSON_PrintUnformatted(held);
    const char *parts[4] = {"the hold of ", host_id, " answered no usable pid: ", answer == NULL ? "" : answer};
    cJSON *refusal = host_refusal_of(parts, 4, HOST_REINSTALL_FIX);
    cJSON_free(answer);
    return refusal;
}

/** Notes the pid held answers for host_id in pids (when given); 0 with *refusal set for an answer without one, or none when memory ran out. */
static int record_pid(cJSON *pids, const char *host_id, const cJSON *held, cJSON **refusal) {
    const cJSON *pid = pid_of(held);
    if (pid == NULL) {
        *refusal = unusable_pid_refusal(host_id, held);
        return 0;
    }
    return pids == NULL || cJSON_AddNumberToObject(pids, host_id, pid->valuedouble) != NULL;
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
        if (held == NULL) {
            release_before(context, starts, start, holder);
            return 0;
        }
        int recorded = record_pid(pids, start->string, held, refusal);
        cJSON_Delete(held);
        if (!recorded) {
            release_before(context, starts, start->next, holder);
            return 0;
        }
    }
    return 1;
}

void host_release_all(const host_context *context, const cJSON *starts, double holder) {
    release_before(context, starts, NULL, holder);
}
