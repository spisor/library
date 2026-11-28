#include <string.h>

#include "allocation.h"
#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "host_references.h"
#include "host_service.h"
#include "interface_start.h"
#include "process_start.h"
#include "terminal_question.h"

#define UI_USAGE "ui takes at most a loader id and --window"

/** What `ui [<loader>] [--window]` asks for. */
typedef struct ui_request {
    const char *loader_id;
    int window;
} ui_request;

/** 1 with request read from the arguments, else 0 with *problem naming what is wrong. */
static int read_request(int count, char **arguments, ui_request *request, const char **problem) {
    request->loader_id = NULL;
    request->window = 0;
    for (int index = 0; index < count; index++) {
        if (strcmp(arguments[index], RUNTIME_INTERFACE_WINDOW_FLAG) == 0 && !request->window) {
            request->window = 1;
        } else if (strncmp(arguments[index], "--", 2) != 0 && request->loader_id == NULL &&
                   runtime_is_plain_host_id(arguments[index])) {
            request->loader_id = arguments[index];
        } else {
            *problem = UI_USAGE;
            return 0;
        }
    }
    return 1;
}

static cJSON *window_refusal(const char *problem) {
    const char *parts[2] = {"the interface's window could not be started: ", problem};
    return host_refusal_of(parts, 2, HOST_REINSTALL_FIX);
}

/**
 * Starts the window detached and answers at once: a window outlives the command that opened it, as an application's
 * user command that runs `spisor-host ui --window` must not wait on it.
 *
 * @implNote Detached means no console at all on Windows (DETACHED_PROCESS), so the console-subsystem interface shows
 * no console window, and every child it needs is C's processes.run_action, which starts it with CREATE_NO_WINDOW.
 */
static int start_window(const host_context *context, const cJSON *start) {
    runtime_detached_start detached = {host_text(start, "executable"), cJSON_GetObjectItemCaseSensitive(start, "arguments"),
                                       cJSON_GetObjectItemCaseSensitive(start, "environment"), context->state};
    runtime_detached_host host;
    char *problem = NULL;
    int started = runtime_start_detached(&detached, &host, &problem) && runtime_release_detached(&host, &problem);
    cJSON *refusal = started || problem == NULL ? NULL : window_refusal(problem);
    spisor_memory_release(problem);
    return started ? HOST_EXIT_DONE : host_refuse(refusal);
}

/** Runs the interface in this terminal, in this process's place, answering its exit code. */
static int run_in_terminal(const cJSON *start) {
    return host_refuse(host_replace_process(host_text(start, "executable"),
                                            cJSON_GetObjectItemCaseSensitive(start, "arguments"), NULL));
}

static int start_interface(const host_context *context, const ui_request *request) {
    cJSON *refusal = NULL;
    int window = request->window || !host_terminal_attached();
    cJSON *start = runtime_interface_start(context->environment, request->loader_id, window, &refusal);
    int code = start == NULL ? host_refuse(refusal) : window ? start_window(context, start) : run_in_terminal(start);
    cJSON_Delete(start);
    return code;
}

int host_command_ui(int count, char **arguments) {
    ui_request request;
    const char *problem = NULL;
    if (!read_request(count, arguments, &request, &problem)) {
        return host_misuse(problem);
    }
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? start_interface(&context, &request) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}
