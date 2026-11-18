#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "file_times.h"
#include "host_context.h"
#include "host_definition.h"
#include "processes_ready.h"
#include "runtime/processes.h"
#include "spisor/allocation.h"
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

/** Publishes the ready record a session host does on every start, through the library as the real one does. */
static void publish_ready(const host_context *context, const char *host_id) {
    const char *configured = host_text(context->environment, "SPISOR_FIXTURE_READY");
    const char *mode = configured == NULL ? "" : configured;
    if (strcmp(mode, "none") == 0) {
        return;
    }
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_ready_argument argument = SPISOR_INIT(runtime_processes_ready_argument);
    cJSON *environment = strcmp(mode, "refuse") == 0 || mode[0] == 0 ? NULL : cJSON_Parse(mode);
    runtime_variable *variables =
        environment == NULL ? NULL : host_variables_of(environment, &argument.environment_count);
    argument.state = context->state;
    argument.host_id = host_id;
    argument.pid = (int64_t)host_process_id();
    argument.environment = variables;
    if (strcmp(mode, "refuse") == 0) {
        argument.refused = "the fixture session host refused";
        argument.fix = "the fixture fix";
    }
    processes->ready(processes->context, &argument, NULL);
    spisor_memory_release(variables);
    cJSON_Delete(environment);
}

static int prune_says_exit(const host_context *context, const char *host_id, double interval) {
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_prune_argument argument = SPISOR_INIT(runtime_processes_prune_argument);
    runtime_processes_prune_answer answer = SPISOR_INIT(runtime_processes_prune_answer);
    argument.state = context->state;
    argument.host_id = host_id;
    argument.self = (int64_t)host_process_id();
    argument.check_milliseconds = interval;
    int exit_now = processes->prune(processes->context, &argument, &answer, NULL) != SPISOR_OK || answer.exit != 0;
    processes->release_prune_answer(processes->context, &answer);
    return exit_now;
}

/**
 * Run as <host-id> action <plugin> <action> <input-json>: prints the arguments after the host id, one per
 * line, and exits with the number after "exit-" when the action starts with it, 0 otherwise.
 */
static int run_action(int count, char **arguments) {
    host_context context;
    cJSON *refusal = NULL;
    if (!host_open_context(&context, &refusal)) {
        cJSON_Delete(refusal);
        return 2;
    }
    record_start(&context, count, arguments);
    for (int index = count - 4; index < count; index++) {
        fputs(arguments[index], stdout);
        fputs("\n", stdout);
    }
    const char *action = arguments[count - 2];
    int code = strncmp(action, "exit-", 5) == 0 ? atoi(action + 5) : 0;
    host_close_context(&context);
    return code;
}

/**
 * A host's language layer as the tests need one, run as a native background host <host-id>, or as the JS
 * runtime the cache records, <entry> <host-id>: loops on processes.prune for the host id it was given, its
 * last argument, until it is told to exit. SPISOR_FIXTURE_START_DELAY milliseconds pass before it records its
 * start, as a slow process start would let them. Asked -p for its version, as install asks a node, it answers one.
 */
static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "-p") == 0) {
        fputs(NODE_VERSION "\n", stdout);
        return 0;
    }
    if (count >= 6 && strcmp(arguments[count - 4], "action") == 0) {
        return run_action(count, arguments);
    }
    host_context context;
    cJSON *refusal = NULL;
    if (count < 2 || !host_open_context(&context, &refusal)) {
        cJSON_Delete(refusal);
        return 2;
    }
    const char *delay = host_text(context.environment, "SPISOR_FIXTURE_START_DELAY");
    if (delay != NULL) {
        pause_milliseconds(atol(delay));
    }
    record_start(&context, count, arguments);
    publish_ready(&context, arguments[count - 1]);
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
