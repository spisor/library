#include "cache.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "hosts.h"

static cJSON *cache_call(const host_context *context) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && cJSON_AddStringToObject(argument, "cache", context->cache) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    return argument;
}

/** Adds member of what function answers for the cache to status; 0 with *refusal set when it refused. */
static int add_cache_answer(cJSON *status, const host_context *context, runtime_json_function function, const char *member,
                            cJSON **refusal) {
    cJSON *answer = host_call(function, cache_call(context), refusal);
    cJSON *value = member == NULL ? answer : cJSON_DetachItemFromObjectCaseSensitive(answer, member);
    if (member != NULL) {
        cJSON_Delete(answer);
    }
    if (value == NULL || !cJSON_AddItemToObject(status, member == NULL ? "installed" : member, value)) {
        cJSON_Delete(value);
        return 0;
    }
    return 1;
}

static int refuse_damaged(const cJSON *damaged) {
    const cJSON *first = cJSON_GetArrayItem(damaged, 0);
    const char *parts[1] = {host_text(first, "detail")};
    return host_refuse(host_refusal_of(parts, 1, host_text(first, "fix")));
}

/**
 * {hosts, unreadable, installed, damaged}: what hosts.list answers, what the cache holds for this platform
 * and what cache.damaged finds, refused when it finds anything.
 *
 * @implNote A start checks only sizes, so status is where a same-size corruption is found.
 */
static int print_status(const host_context *context) {
    cJSON *refusal = NULL;
    cJSON *status = host_call(runtime_hosts_list, host_places_argument(context), &refusal);
    if (status == NULL || !add_cache_answer(status, context, runtime_cache_installed, NULL, &refusal) ||
        !add_cache_answer(status, context, runtime_cache_damaged, "damaged", &refusal)) {
        cJSON_Delete(status);
        return host_refuse(refusal);
    }
    host_print_json(status);
    const cJSON *damaged = cJSON_GetObjectItemCaseSensitive(status, "damaged");
    int code = cJSON_GetArraySize(damaged) == 0 ? HOST_EXIT_DONE : refuse_damaged(damaged);
    cJSON_Delete(status);
    return code;
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
