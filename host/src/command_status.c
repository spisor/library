#include "host_commands.h"
#include "host_context.h"
#include "host_definition.h"
#include "host_output.h"

static int add_answer(cJSON *status, const char *member, cJSON *answer) {
    if (answer == NULL || !cJSON_AddItemToObject(status, member, answer)) {
        cJSON_Delete(answer);
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
    cJSON *status = host_listing_of(context->state, &refusal);
    if (status == NULL || !add_answer(status, "installed", host_installed_of(context->cache, &refusal)) ||
        !add_answer(status, "damaged", host_damaged_of(context->cache, &refusal))) {
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
