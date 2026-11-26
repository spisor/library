#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "allocation.h"
#include "directory_tree.h"
#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_options.h"
#include "host_output.h"
#include "host_start.h"
#include "paths.h"
#include "run_result.h"
#include "runtime/hosts.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define ACTION_USAGE "action needs the host id, the plugin and the action, and takes --input name=value as often as it needs"

static char *problem_of(const char *first, const char *second) {
    const char *parts[2] = {first, second};
    return runtime_cache_text(parts, 2);
}

/** Adds one --input value, name=value, to input as a string; 1, or 0 with *problem set to text for the caller to release (NULL when memory ran out). */
static int add_input(cJSON *input, const char *pair, char **problem) {
    const char *equals = strchr(pair, '=');
    if (equals == NULL || equals == pair) {
        *problem = problem_of("--input takes name=value, not ", pair);
        return 0;
    }
    char *name = spisor_memory_allocate((size_t)(equals - pair) + 1);
    if (name == NULL) {
        return 0;
    }
    memcpy(name, pair, (size_t)(equals - pair));
    name[equals - pair] = 0;
    int added = 0;
    if (cJSON_HasObjectItem(input, name)) {
        *problem = problem_of("--input names the same input twice: ", name);
    } else {
        added = cJSON_AddStringToObject(input, name, equals + 1) != NULL;
    }
    spisor_memory_release(name);
    return added;
}

/** The object of every --input pair as compact JSON, freed with cJSON_free; NULL with *problem as add_input leaves it. */
static char *input_json(const cJSON *pairs, char **problem) {
    cJSON *input = cJSON_CreateObject();
    *problem = NULL;
    int added = input != NULL;
    for (const cJSON *pair = pairs == NULL ? NULL : pairs->child; added && pair != NULL; pair = pair->next) {
        added = add_input(input, pair->valuestring, problem);
    }
    char *text = added ? cJSON_PrintUnformatted(input) : NULL;
    cJSON_Delete(input);
    return text;
}

/** Whether host_id is a loader that records no start of its own, whose plugins run in its session host. */
static int runs_in_session_host(const host_context *context, const char *host_id) {
    cJSON *refusal = NULL;
    cJSON *registration = host_registration(context, host_id, &refusal);
    int loader = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(registration, "application")) &&
                 !cJSON_HasObjectItem(registration, "start");
    cJSON_Delete(registration);
    cJSON_Delete(refusal);
    return loader;
}

/** A path for this process's next result, `<state>/results/<pid>-<n>.json`, freed; NULL when memory runs out. */
static char *next_result_path(const host_context *context) {
    static unsigned long taken;
    char name[64];
    snprintf(name, sizeof name, "%.0f-%lu.json", host_process_id(), ++taken);
    const char *segments[3] = {context->state, RUNTIME_RESULTS_FOLDER, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

/** Prints the result the action's run wrote to result, when it wrote one, and removes the file; 0 out of memory. */
static int answer_result(const char *result) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    char *text = (char *)spisor_read_file_reporting(result, &length, &outcome);
    if (text != NULL) {
        host_write(0, text);
        host_write(0, "\n");
    }
    spisor_memory_release(text);
    return runtime_remove_file_entry(result) != RUNTIME_ENTRY_OUT_OF_MEMORY && outcome != SPISOR_READ_OUT_OF_MEMORY;
}

/** The one layer hosts' layer_start answers for an action of a loader's plugin; NULL with *refusal set. */
static cJSON *action_layer(const host_context *context, const char *loader_id, const char *const *action_arguments,
                           const char *result, cJSON **refusal) {
    cJSON *layers = host_layer_starts(context, loader_id, RUNTIME_HOSTS_LAYER_ACTION, action_arguments[1],
                                      action_arguments[2], action_arguments[3], result, refusal);
    cJSON *layer = cJSON_DetachItemFromArray(layers, 0);
    cJSON_Delete(layers);
    return layer;
}

/**
 * Runs the start the registration of host_id records, or the layer of a loader's plugin when it records none,
 * with the arguments action, plugin, action and input, as this process's child sharing its terminal, and
 * answers its exit code. A loader's layer is also given a result path of this process's own, whose result is
 * printed once the child exits and the file removed before answering.
 *
 * @implNote A background host's recorded start is the archived host's, which takes no result path.
 */
static int run_action(const host_context *context, const char *host_id, const char *const *action_arguments) {
    cJSON *refusal = NULL;
    int in_session_host = runs_in_session_host(context, host_id);
    char *result = in_session_host ? next_result_path(context) : NULL;
    cJSON *start = NULL;
    if (in_session_host && result == NULL) {
        return host_refuse(NULL);
    }
    start = in_session_host ? action_layer(context, host_id, action_arguments, result, &refusal)
                            : host_start_of(context, host_id, action_arguments, 4, &refusal);
    int exit_code = 1;
    int ran = start != NULL &&
              host_run_child(host_text(start, "executable"), cJSON_GetObjectItemCaseSensitive(start, "arguments"),
                             cJSON_GetObjectItemCaseSensitive(start, "environment"), NULL, &exit_code, &refusal);
    int answered = result == NULL || answer_result(result);
    cJSON_Delete(start);
    spisor_memory_release(result);
    if (!answered) {
        cJSON_Delete(refusal);
        return host_refuse(NULL);
    }
    return ran ? exit_code : host_refuse(refusal);
}

static int run_in_context(const char *host_id, const char *const *action_arguments) {
    host_context context;
    cJSON *refusal = NULL;
    int code = host_open_context(&context, &refusal) ? run_action(&context, host_id, action_arguments) : host_refuse(refusal);
    host_close_context(&context);
    return code;
}

static int misuse_and_free(char *problem) {
    int code = host_misuse(problem == NULL ? "out of memory" : problem);
    spisor_memory_release(problem);
    return code;
}

int host_command_action(int count, char **arguments) {
    host_option options[1] = {{"input", NULL}};
    cJSON *positionals = cJSON_CreateArray();
    char *problem = NULL;
    int parsed = positionals != NULL && host_parse_options(count, arguments, options, 1, positionals, &problem);
    int code = HOST_EXIT_MISUSED;
    if (!parsed) {
        code = misuse_and_free(problem);
    } else if (cJSON_GetArraySize(positionals) != 3) {
        code = host_misuse(ACTION_USAGE);
    } else {
        char *input = input_json(options[0].values, &problem);
        const char *action_arguments[4] = {"action", cJSON_GetArrayItem(positionals, 1)->valuestring,
                                           cJSON_GetArrayItem(positionals, 2)->valuestring, input};
        code = input == NULL ? misuse_and_free(problem)
                             : run_in_context(cJSON_GetArrayItem(positionals, 0)->valuestring, action_arguments);
        cJSON_free(input);
    }
    host_free_options(options, 1);
    cJSON_Delete(positionals);
    return code;
}
