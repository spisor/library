#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "file_times.h"
#include "host_context.h"
#include "processes.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <windows.h>

#include "host_arguments.h"
#else
#include <time.h>
#endif

/** No fixture outlives a broken test by more than this. */
#define LONGEST_LIFE_MILLISECONDS 60000.0
#define DEFAULT_INTERVAL 300.0
#define NODE_VERSION "22.0.0"

static void pause_milliseconds(long milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

/** Appends "<pid> <arguments...>" to the file SPISOR_FIXTURE_STARTS names, so a test sees every layer started. */
static void record_start(const host_context *context, int count, char **arguments) {
    const char *starts = host_text(context->environment, "SPISOR_FIXTURE_STARTS");
    if (starts == NULL) {
        return;
    }
    char line[8192];
    size_t used = (size_t)snprintf(line, sizeof line, "%.0f", host_process_id());
    for (int index = 1; index < count && used < sizeof line; index++) {
        used += (size_t)snprintf(line + used, sizeof line - used, " %s", arguments[index]);
    }
    if (used + 1 < sizeof line) {
        line[used++] = '\n';
        spisor_append_file(starts, (const uint8_t *)line, used);
    }
}

static int prune_says_exit(const host_context *context, const char *host_id, double interval) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", context->state);
    cJSON_AddStringToObject(argument, "hostId", host_id);
    cJSON_AddNumberToObject(argument, "self", host_process_id());
    cJSON_AddNumberToObject(argument, "checkMilliseconds", interval);
    cJSON *refusal = NULL;
    cJSON *answer = host_call(runtime_processes_prune, argument, &refusal);
    int exit_now = answer == NULL || !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer, "exit"));
    cJSON_Delete(answer);
    cJSON_Delete(refusal);
    return exit_now;
}

/**
 * A host's language layer as the tests need one, run as a native background host <host-id>, or as the JS
 * runtime the cache records, <entry> <host-id>: loops on processes.prune for the host id it was given, its
 * last argument, until it is told to exit. Asked -p for its version, as install asks a node, it answers one.
 */
static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "-p") == 0) {
        fputs(NODE_VERSION "\n", stdout);
        return 0;
    }
    host_context context;
    cJSON *refusal = NULL;
    if (count < 2 || !host_open_context(&context, &refusal)) {
        cJSON_Delete(refusal);
        return 2;
    }
    record_start(&context, count, arguments);
    const char *configured = host_text(context.environment, "SPISOR_FIXTURE_INTERVAL");
    double interval = configured == NULL ? DEFAULT_INTERVAL : atof(configured);
    double started = runtime_now_milliseconds();
    int code = 3;
    while (runtime_now_milliseconds() - started < LONGEST_LIFE_MILLISECONDS) {
        if (prune_says_exit(&context, arguments[count - 1], interval)) {
            code = 0;
            break;
        }
        pause_milliseconds((long)interval);
    }
    host_close_context(&context);
    return code;
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = host_utf8_arguments(count, wide);
    int code = arguments == NULL ? 2 : run(count, arguments);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    return run(count, arguments);
}

#endif
