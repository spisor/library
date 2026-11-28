#include "processes_child_start.h"
#include "allocation.h"

#include <stdio.h>
#include <string.h>

#include "hosts_layer_start.h"
#include "json_service.h"
#include "places.h"
#include "process_run.h"
#include "registry.h"
#include "run_result.h"
#include "runtime/hosts.h"
#include "spisor/json_kind.h"

#define PLACES_MESSAGE "the cache and state the call names are not the places its environment resolves to"
#define PLACES_FIX "pass the cache and state that places' resolve answers for the same environment"
#define UNREADABLE_RESULT_MESSAGE "the result the action's run wrote could not be read as JSON"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int is_same_text(const char *left, const char *right) {
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

/** Whether the environment's places are the request's cache and state; 0 with *refusal set, NULL out of memory. */
static int places_agree(const runtime_child_action_request *request, cJSON **refusal) {
    cJSON *places = runtime_places_of_environment(request->environment, refusal);
    int agree = places != NULL && is_same_text(text_of(places, "cache"), request->cache) &&
                is_same_text(text_of(places, "state"), request->state);
    if (places != NULL && !agree) {
        *refusal = runtime_refusal(PLACES_MESSAGE, PLACES_FIX);
    }
    cJSON_Delete(places);
    return agree;
}

/** The one layer hosts.layer_start answers for the ACTION, its result written to result; NULL with *refusal. */
static cJSON *action_layer(const runtime_child_action_request *request, const char *result, cJSON **refusal) {
    runtime_hosts_layer_start_request start = {request->environment, request->loader_id, RUNTIME_HOSTS_LAYER_ACTION,
                                               request->plugin,      request->action,    request->input,
                                               result};
    cJSON *answer = runtime_hosts_layer_start(&start, refusal);
    cJSON *layer = cJSON_DetachItemFromArray(cJSON_GetObjectItemCaseSensitive(answer, "layers"), 0);
    cJSON_Delete(answer);
    return layer;
}

static int is_blank(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

/** errors without the blanks around it, else a sentence naming exit_code when nothing is left, freed; NULL out of memory. */
static char *failure_of(const char *errors, int exit_code) {
    const char *start = errors == NULL ? "" : errors;
    size_t length;
    while (is_blank(*start)) {
        start++;
    }
    length = strlen(start);
    while (length > 0 && is_blank(start[length - 1])) {
        length--;
    }
    char *failure = spisor_memory_allocate(length > 0 ? length + 1 : 64);
    if (failure != NULL && length > 0) {
        memcpy(failure, start, length);
        failure[length] = '\0';
    } else if (failure != NULL) {
        snprintf(failure, 64, "the action's run exited with code %d", exit_code);
    }
    return failure;
}

/** text, the result the run wrote, as JSON; NULL with *refusal set when it is none, or with it NULL out of memory. */
static cJSON *result_value(const char *text, cJSON **refusal) {
    int out_of_memory = 0;
    cJSON *value = spisor_parse_json_reporting((const uint8_t *)text, strlen(text), &out_of_memory);
    if (value == NULL && !out_of_memory) {
        *refusal = runtime_refusal(UNREADABLE_RESULT_MESSAGE, RUNTIME_REINSTALL_FIX);
    }
    return value;
}

/** {exitCode, result?, failure?}; NULL with *refusal set as result_value sets it, or with it NULL out of memory. */
static cJSON *answer_of(int exit_code, const char *text, const char *errors, cJSON **refusal) {
    cJSON *value = text == NULL ? NULL : result_value(text, refusal);
    char *failure = exit_code == 0 ? NULL : failure_of(errors, exit_code);
    cJSON *answer = text != NULL && value == NULL ? NULL : cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddNumberToObject(answer, "exitCode", exit_code) != NULL &&
                (exit_code == 0 || (failure != NULL && cJSON_AddStringToObject(answer, "failure", failure) != NULL));
    if (built && value != NULL) {
        built = cJSON_AddItemToObject(answer, "result", value);
        value = built ? NULL : value;
    }
    if (!built) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    cJSON_Delete(value);
    spisor_memory_release(failure);
    return answer;
}

/**
 * Runs the layer's start and answers what it did. The result file is removed whatever happened once the start was
 * attempted, since a child that ran may have written it before memory ran out.
 */
static cJSON *run_layer(const cJSON *layer, const char *result, cJSON **refusal) {
    int exit_code = 1;
    int out_of_memory = 0;
    char *errors = NULL;
    char *problem = NULL;
    int ran = runtime_run_reporting_errors(text_of(layer, "executable"),
                                           cJSON_GetObjectItemCaseSensitive(layer, "arguments"),
                                           cJSON_GetObjectItemCaseSensitive(layer, "environment"), &exit_code,
                                           &errors, &problem);
    char *text = runtime_take_result(result, ran ? exit_code : 1, &out_of_memory);
    cJSON *answer = NULL;
    if (!ran && problem != NULL) {
        *refusal = runtime_refusal(problem, RUNTIME_REINSTALL_FIX);
    } else if (ran && !out_of_memory) {
        answer = answer_of(exit_code, text, errors, refusal);
    }
    spisor_memory_release(text);
    spisor_memory_release(problem);
    spisor_memory_release(errors);
    return answer;
}

cJSON *runtime_processes_run_action(const runtime_child_action_request *request, cJSON **refusal) {
    int removable = 1;
    char *result = NULL;
    cJSON *layer = NULL;
    cJSON *answer = NULL;
    *refusal = NULL;
    if (!places_agree(request, refusal)) {
        return NULL;
    }
    result = runtime_fresh_result_path(request->state, &removable);
    if (result == NULL && !removable) {
        *refusal = runtime_refusal(RUNTIME_STALE_RESULT_MESSAGE, RUNTIME_STALE_RESULT_FIX);
    }
    layer = result == NULL ? NULL : action_layer(request, result, refusal);
    answer = layer == NULL ? NULL : run_layer(layer, result, refusal);
    cJSON_Delete(layer);
    spisor_memory_release(result);
    return answer;
}
