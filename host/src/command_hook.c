#include <string.h>

#include "cache_refusal.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_output.h"
#include "loader_start.h"

#define SESSION_START "session-start"

static int answer(const char *loader_id, const char *event, cJSON **refusal) {
    if (strcmp(event, SESSION_START) != 0) {
        cJSON *quoted = cJSON_CreateString(event);
        char *printed = quoted == NULL ? NULL : cJSON_PrintUnformatted(quoted);
        const char *parts[3] = {"the hook event ", printed == NULL ? event : printed, " is not one this host answers"};
        *refusal = runtime_cache_refusal(parts, 3, "run the hook with one of: " SESSION_START);
        cJSON_free(printed);
        cJSON_Delete(quoted);
        return 0;
    }
    host_context context;
    loader_start loader;
    memset(&loader, 0, sizeof loader);
    int answered = host_open_context(&context, refusal) && host_start_loader(&context, loader_id, &loader, refusal);
    host_finish_loader(&loader);
    host_close_context(&context);
    return answered;
}

/**
 * Answers one event of the application's own plugin mechanism for loader_id: starts the loader as every
 * start does, registering it and requesting its commands again and recording the version it runs, and
 * starts no process at all.
 *
 * @implNote Silent on success and exiting 0 whatever happens, because the application runs this as it
 * starts: an application reads a start hook's stdout as context, and a spisor problem must never stop
 * the application starting. A refusal is written to stderr with its fix.
 */
int host_command_hook(int count, char **arguments) {
    if (count != 2) {
        return host_misuse("hook needs exactly a loader id and an event");
    }
    cJSON *refusal = NULL;
    if (!answer(arguments[0], arguments[1], &refusal)) {
        host_print_refusal(refusal);
        cJSON_Delete(refusal);
    }
    return HOST_EXIT_DONE;
}
