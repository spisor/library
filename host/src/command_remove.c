#include "host_commands.h"
#include "host_context.h"
#include "host_definition.h"
#include "host_output.h"

/**
 * Removes host id through hosts.remove, then sweeps the cache of what nothing references any more, and
 * prints {removed, swept}.
 *
 * @implNote The application a loader serves is never removed, only what spisor did around it.
 */
static int remove_host(const host_context *context, const char *id) {
    cJSON *refusal = NULL;
    cJSON *removed = host_removal_of(context->state, context->cache, id, &refusal);
    cJSON *swept = removed == NULL ? NULL : host_sweep_of(context->cache, context->state, &refusal);
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
