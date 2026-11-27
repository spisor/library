#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog_patterns.h"
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
#define NODE_VERSION "22.18.0"

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

/**
 * The layer's language when it runs as `<runtime binary> <language> <loader id>`, as every layer start ends; NULL for a
 * background host, started with its id alone.
 */
static const char *layer_language(int count, char **arguments) {
    const char *language = count >= 4 ? arguments[count - 2] : NULL;
    return runtime_catalog_is_language(language) ? language : NULL;
}

/**
 * The identity the fixture publishes and is held under, as a real run's is: the loader's for the layer whose language
 * SPISOR_FIXTURE_FRONT_DOOR names, typescript by default, `<loader>.<language>` for any other layer, and a background
 * host's own id.
 */
static char *identity_of(const host_context *context, int count, char **arguments) {
    const char *language = layer_language(count, arguments);
    const char *configured = host_text(context->environment, "SPISOR_FIXTURE_FRONT_DOOR");
    const char *door = configured == NULL ? "typescript" : configured;
    const char *host_id = arguments[count - 1];
    int own = language == NULL || strcmp(language, door) == 0;
    size_t length = strlen(host_id) + (own ? 0 : strlen(language) + 1) + 1;
    char *identity = spisor_memory_allocate(length);
    if (identity != NULL) {
        snprintf(identity, length, own ? "%s" : "%s.%s", host_id, language);
    }
    return identity;
}

/** Appends `@<language>` to every value of environment, so a test reads which layer published what it sees. */
static void tag_with_language(cJSON *environment, const char *language) {
    cJSON *variable;
    cJSON_ArrayForEach(variable, environment) {
        char tagged[1024];
        if (cJSON_IsString(variable)) {
            snprintf(tagged, sizeof tagged, "%s@%s", variable->valuestring, language);
            cJSON_SetValuestring(variable, tagged);
        }
    }
}

/**
 * Publishes the ready record a session host does on every start, through the library as the real one does: a refusal
 * when SPISOR_FIXTURE_READY is refuse or SPISOR_FIXTURE_REFUSING names the layer's language, each value tagged with
 * the layer's language when SPISOR_FIXTURE_TAG_LAYER is set.
 */
static void publish_ready(const host_context *context, const char *host_id, const char *language) {
    const char *configured = host_text(context->environment, "SPISOR_FIXTURE_READY");
    const char *refusing = host_text(context->environment, "SPISOR_FIXTURE_REFUSING");
    const char *mode = refusing != NULL && language != NULL && strcmp(refusing, language) == 0 ? "refuse"
                       : configured == NULL                                                 ? ""
                                                                                            : configured;
    if (strcmp(mode, "none") == 0) {
        return;
    }
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_ready_argument argument = SPISOR_INIT(runtime_processes_ready_argument);
    cJSON *environment = strcmp(mode, "refuse") == 0 || mode[0] == 0 ? NULL : cJSON_Parse(mode);
    if (environment != NULL && language != NULL && host_text(context->environment, "SPISOR_FIXTURE_TAG_LAYER") != NULL) {
        tag_with_language(environment, language);
    }
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

#define FIXTURE_RESULT "{\"fixture\":\"result\"}"

/**
 * Run as <host-id> action <plugin> <action> <input-json>, the result's path after the input when `with_result`:
 * prints the arguments from the action word to the input, one per line, writes FIXTURE_RESULT to the result's path
 * unless the action starts with "refuse-", which then exits 1 as a run whose write was refused does, and exits with
 * the number after "exit-" when the action starts with it, 0 otherwise.
 */
static int run_action(int count, char **arguments, int with_result) {
    host_context context;
    cJSON *refusal = NULL;
    int last = with_result ? count - 1 : count;
    if (!host_open_context(&context, &refusal)) {
        cJSON_Delete(refusal);
        return 2;
    }
    record_start(&context, count, arguments);
    for (int index = last - 4; index < last; index++) {
        fputs(arguments[index], stdout);
        fputs("\n", stdout);
    }
    const char *action = arguments[last - 2];
    int refused = strncmp(action, "refuse-", 7) == 0;
    if (with_result && !refused) {
        spisor_append_file(arguments[count - 1], (const uint8_t *)FIXTURE_RESULT, strlen(FIXTURE_RESULT));
    }
    int code = refused ? 1 : strncmp(action, "exit-", 5) == 0 ? atoi(action + 5) : 0;
    host_close_context(&context);
    return code;
}

/**
 * A host's language layer as the tests need one, run as a native background host <host-id>, as a native layer
 * <runtime binary> <language> <loader id>, or as the JS runtime the cache records, <entry> <runtime binary> <language>
 * <loader id>: loops on processes.prune for its identity until it is told to exit. SPISOR_FIXTURE_START_DELAY milliseconds pass before it records its
 * start, as a slow process start would let them. Asked -p for its version, as install asks a node, it answers one.
 */
static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "-p") == 0) {
        fputs(NODE_VERSION "\n", stdout);
        return 0;
    }
    if (count >= 6 && strcmp(arguments[count - 4], "action") == 0) {
        return run_action(count, arguments, 0);
    }
    if (count >= 7 && strcmp(arguments[count - 5], "action") == 0) {
        return run_action(count, arguments, 1);
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
    char *identity = identity_of(&context, count, arguments);
    if (identity == NULL) {
        host_close_context(&context);
        return 2;
    }
    publish_ready(&context, identity, layer_language(count, arguments));
    const char *configured = host_text(context.environment, "SPISOR_FIXTURE_INTERVAL");
    double interval = configured == NULL ? DEFAULT_INTERVAL : atof(configured);
    double started = runtime_now_milliseconds();
    int code = 3;
    while (runtime_now_milliseconds() - started < LONGEST_LIFE_MILLISECONDS) {
        if (prune_says_exit(&context, identity, interval)) {
            code = 0;
            break;
        }
        pause_milliseconds((long)interval);
    }
    spisor_memory_release(identity);
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
