#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "host_child.h"
#include "host_commands.h"
#include "host_context.h"
#include "host_options.h"
#include "host_output.h"
#include "host_start.h"

#define ACTION_USAGE "action needs the host id, the plugin and the action, and takes --input name=value as often as it needs"

static char *problem_of(const char *first, const char *second) {
    const char *parts[2] = {first, second};
    return runtime_cache_text(parts, 2);
}

/** Adds one --input value, name=value, to input as a string; 1, or 0 with *problem set to text for the caller to free (NULL when memory ran out). */
static int add_input(cJSON *input, const char *pair, char **problem) {
    const char *equals = strchr(pair, '=');
    if (equals == NULL || equals == pair) {
        *problem = problem_of("--input takes name=value, not ", pair);
        return 0;
    }
    char *name = malloc((size_t)(equals - pair) + 1);
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
    free(name);
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

/**
 * Runs the start the registration of host_id records, or a loader's session host when it records none,
 * with the arguments action, plugin, action and input, as this process's child sharing its terminal, and
 * answers its exit code.
 */
static int run_action(const host_context *context, const char *host_id, const char *const *action_arguments) {
    cJSON *refusal = NULL;
    cJSON *start = runs_in_session_host(context, host_id) ? host_session_start_of(context, host_id, &refusal)
                                                          : host_start_of(context, host_id, &refusal);
    if (start == NULL || !host_start_append_arguments(start, action_arguments, 4)) {
        cJSON_Delete(start);
        return host_refuse(refusal);
    }
    int exit_code = 1;
    int ran = host_run_child(host_text(start, "executable"), cJSON_GetObjectItemCaseSensitive(start, "arguments"),
                             cJSON_GetObjectItemCaseSensitive(start, "environment"), NULL, &exit_code, &refusal);
    cJSON_Delete(start);
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
    free(problem);
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
