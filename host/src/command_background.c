#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "host_start.h"
#include "own_run.h"

/**
 * Runs background host id in its own layer through the start its registration records, in this process's
 * place: what a launcher's hold starts, run by hand.
 *
 * @implNote The layer itself loops on processes.prune and exits once no holder is alive, so with nothing
 * holding it, it stops at its first check.
 */
#if defined(_WIN32)
#define HOST_REPLACING_KEEPS_THIS_PROCESS 1
#else
#define HOST_REPLACING_KEEPS_THIS_PROCESS 0
#endif

static void stamp(void *context) {
    host_stamp_own_run(context);
}

/**
 * @implNote On Windows this process waits for the layer for as long as it runs, so it records the
 * forebay-host version it runs as a launch does; on POSIX execv leaves no forebay-host behind to record.
 */
static int run_background(const host_context *context, const char *id) {
    cJSON *refusal = NULL;
    cJSON *start = host_start_of(context, id, &refusal);
    if (start == NULL) {
        return host_refuse(refusal);
    }
    own_run run = {NULL, NULL, NULL};
    if (HOST_REPLACING_KEEPS_THIS_PROCESS) {
        host_start_own_run(&run, context);
    }
    host_child_watch watch = {NULL, stamp, HOST_OWN_RUN_INTERVAL_MILLISECONDS, &run};
    refusal = host_replace_process(host_text(start, "executable"), cJSON_GetObjectItemCaseSensitive(start, "arguments"),
                                   run.record == NULL ? NULL : &watch);
    host_finish_own_run(&run);
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
