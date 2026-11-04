#include "cache.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "hosts.h"

static cJSON *remove_call(const host_context *context, const char *id) {
    cJSON *call = host_places_argument(context);
    if (call != NULL && cJSON_AddStringToObject(call, "id", id) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static cJSON *sweep_call(const host_context *context) {
    cJSON *call = cJSON_CreateObject();
    if (call != NULL && (cJSON_AddStringToObject(call, "cache", context->cache) == NULL ||
                         cJSON_AddStringToObject(call, "state", context->state) == NULL)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/**
 * Removes host id through hosts.remove, then sweeps the cache of what nothing references any more, and
 * prints {removed, swept}.
 *
 * @implNote The application a loader serves is never removed, only what spisor did around it.
 */
static int remove_host(const host_context *context, const char *id) {
    cJSON *refusal = NULL;
    cJSON *removed = host_call(runtime_hosts_remove, remove_call(context, id), &refusal);
    cJSON *swept = removed == NULL ? NULL : host_call(runtime_cache_sweep, sweep_call(context), &refusal);
    cJSON *summary = swept == NULL ? NULL : cJSON_CreateObject();
    if (summary == NULL || !cJSON_AddItemToObject(summary, "removed", removed)) {
        cJSON_Delete(summary);
        cJSON_Delete(swept);
        cJSON_Delete(removed);
        return host_refuse(refusal);
    }
    cJSON_AddItemToObject(summary, "swept", swept);
    host_print_json(summary);
    cJSON_Delete(summary);
    return HOST_EXIT_DONE;
}

int host_command_remove(int count, char **arguments) {
    if (count != 1 || arguments[0][0] == 0) {
        return host_misuse("remove needs exactly one host id");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? remove_host(&context, arguments[0]) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
