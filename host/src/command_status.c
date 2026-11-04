#include "cache.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "hosts.h"

static cJSON *installed_of(const host_context *context, cJSON **refusal) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", context->cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return host_call(runtime_cache_installed, argument, refusal);
}

/** {hosts, unreadable, installed}: what hosts.list answers and what the cache holds for this platform. */
static int print_status(const host_context *context) {
    cJSON *refusal = NULL;
    cJSON *status = host_call(runtime_hosts_list, host_places_argument(context), &refusal);
    cJSON *installed = status == NULL ? NULL : installed_of(context, &refusal);
    if (installed == NULL || !cJSON_AddItemToObject(status, "installed", installed)) {
        cJSON_Delete(installed);
        cJSON_Delete(status);
        return host_refuse(refusal);
    }
    host_print_json(status);
    cJSON_Delete(status);
    return HOST_EXIT_DONE;
}

int host_command_status(int count, char **arguments) {
    (void)arguments;
    if (count != 0) {
        return host_misuse("status takes no arguments");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? print_status(&context) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
