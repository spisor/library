#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "host_start.h"

/**
 * Runs background host id in its own layer through the start its registration records, in this process's
 * place: what a launcher's hold starts, run by hand.
 *
 * @implNote The layer itself loops on processes.prune and exits once no holder is alive, so with nothing
 * holding it, it stops at its first check.
 */
static int run_background(const host_context *context, const char *id) {
    cJSON *refusal = NULL;
    cJSON *start = host_start_of(context, id, &refusal);
    if (start == NULL) {
        return host_refuse(refusal);
    }
    refusal = host_replace_process(host_text(start, "executable"), cJSON_GetObjectItemCaseSensitive(start, "arguments"));
    cJSON_Delete(start);
    return host_refuse(refusal);
}

int host_command_background(int count, char **arguments) {
    if (count != 1 || arguments[0][0] == 0) {
        return host_misuse("background needs exactly one host id");
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? run_background(&context, arguments[0]) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
