#include "host_start.h"
#include "allocation.h"

#include "host_definition.h"
#include "host_output.h"
#include "hosts_layer_start.h"
#include "runtime/hosts.h"

static cJSON *arguments_of(const char *id, const char *const *extra, size_t count) {
    cJSON *arguments = cJSON_CreateArray();
    int built = arguments != NULL && cJSON_AddItemToArray(arguments, cJSON_CreateString(id));
    for (size_t index = 0; built && index < count; index++) {
        built = cJSON_AddItemToArray(arguments, cJSON_CreateString(extra[index]));
    }
    if (!built) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

cJSON *host_start_of(const host_context *context, const char *id, const char *const *extra, size_t count,
                     cJSON **refusal) {
    cJSON *registration = host_registration(context, id, refusal);
    const cJSON *recorded = cJSON_GetObjectItemCaseSensitive(registration, "start");
    cJSON *arguments = cJSON_IsObject(recorded) ? arguments_of(id, extra, count) : NULL;
    runtime_entry_start_request request = {context->cache, context->environment, id,
                                           host_text(registration, "version"), host_text(recorded, "runtime"),
                                           host_text(recorded, "entry"), arguments};
    cJSON *start = arguments == NULL ? NULL : runtime_entry_start(&request, refusal);
    if (registration != NULL && !cJSON_IsObject(recorded)) {
        const char *parts[3] = {"the background host ", id, " records no start, so it cannot be started"};
        *refusal = host_refusal_of(parts, 3, HOST_REINSTALL_FIX);
    }
    cJSON_Delete(arguments);
    cJSON_Delete(registration);
    return start;
}

static cJSON *layer_json(const runtime_hosts_layer *layer) {
    cJSON *json = cJSON_CreateObject();
    cJSON *arguments = json == NULL ? NULL : cJSON_AddArrayToObject(json, "arguments");
    int built = arguments != NULL && cJSON_AddStringToObject(json, "runtime", layer->runtime) != NULL &&
                cJSON_AddStringToObject(json, "hostId", layer->host_id) != NULL &&
                cJSON_AddStringToObject(json, "executable", layer->executable) != NULL;
    for (size_t index = 0; built && index < layer->argument_count; index++) {
        built = cJSON_AddItemToArray(arguments, cJSON_CreateString(layer->arguments[index]));
    }
    cJSON *environment = built ? host_variables_json(layer->environment, layer->environment_count) : NULL;
    if (environment == NULL || !cJSON_AddItemToObject(json, "environment", environment)) {
        cJSON_Delete(environment);
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static cJSON *layers_json(const runtime_hosts_layer_start_answer *answer) {
    cJSON *layers = cJSON_CreateArray();
    for (size_t index = 0; layers != NULL && index < answer->layer_count; index++) {
        cJSON *layer = layer_json(&answer->layers[index]);
        if (layer == NULL || !cJSON_AddItemToArray(layers, layer)) {
            cJSON_Delete(layer);
            cJSON_Delete(layers);
            layers = NULL;
        }
    }
    return layers;
}

cJSON *host_layer_starts(const host_context *context, const char *loader_id, uint32_t mode, const char *plugin,
                         const char *action, const char *input, cJSON **refusal) {
    const runtime_hosts_definition *hosts = runtime_hosts_definition_of();
    runtime_hosts_layer_start_argument argument = SPISOR_INIT(runtime_hosts_layer_start_argument);
    runtime_hosts_layer_start_answer answer = SPISOR_INIT(runtime_hosts_layer_start_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    runtime_variable *environment = host_variables_of(context->environment, &argument.environment_count);
    argument.environment = environment;
    argument.loader_id = loader_id;
    argument.mode = mode;
    argument.plugin = plugin;
    argument.action = action;
    argument.input = input;
    spisor_status status = environment == NULL ? SPISOR_OUT_OF_MEMORY
                                               : hosts->layer_start(hosts->context, &argument, &answer, &declined);
    cJSON *layers = status == SPISOR_OK ? layers_json(&answer) : NULL;
    *refusal = status == SPISOR_OK || status == SPISOR_OUT_OF_MEMORY ? NULL : host_refusal_json(&declined);
    hosts->release_layer_start_answer(hosts->context, &answer);
    hosts->release_refusal(hosts->context, &declined);
    spisor_memory_release(environment);
    return layers;
}
