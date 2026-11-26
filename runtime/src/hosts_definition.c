#include "runtime/hosts.h"

#include <math.h>
#include <string.h>

#include "answer_storage.h"
#include "catalog_time.h"
#include "changes_definition.h"
#include "definition_support.h"
#include "hosts.h"
#include "hosts_layer_start.h"
#include "hosts_start.h"
#include "install_definition.h"
#include "json_service.h"
#include "spisor/json_value.h"
#include "registry_definition.h"
#include "updates_definition.h"

#define HEADER "the hosts header"
#define REGISTER_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_register_argument, environment_count)
#define REGISTER_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_install_register_loader_answer, storage)
#define REMOVE_SHAPE SPISOR_SHAPE_THROUGH(runtime_registry_forget_argument, id)
#define REMOVE_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_remove_answer, storage)
#define PROVENANCE_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_provenance_argument, now)
#define PROVENANCE_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_provenance_answer, storage)
#define START_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_start_argument, session)
#define START_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_start_answer, storage)
#define LAYER_START_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_layer_start_argument, input)
#define LAYER_START_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_layer_start_answer, storage)
#define LAYER_START_RESULT_SHAPE SPISOR_SHAPE_THROUGH(runtime_hosts_layer_start_argument, result)

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static uint32_t flag_of(const cJSON *object, const char *name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, name)) ? 1 : 0;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

/** Deletes `call` and answers NULL unless `built`. */
static cJSON *built_call(cJSON *call, int built) {
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int add_environment(cJSON *object, const runtime_variable *environment, size_t count) {
    cJSON *json = NULL;
    if (runtime_environment_json(environment, count, &json) != SPISOR_OK ||
        !cJSON_AddItemToObject(object, "environment", json)) {
        cJSON_Delete(json);
        return 0;
    }
    return 1;
}

static int add_texts(cJSON *object, const char *name, const char *const *texts, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, name);
    int built = array != NULL;
    for (size_t index = 0; built && index < count; index++) {
        cJSON *text = cJSON_CreateString(texts[index]);
        built = text != NULL && cJSON_AddItemToArray(array, text);
        if (!built) {
            cJSON_Delete(text);
        }
    }
    return built;
}

/** {places: {state, cache?}}; NULL when memory runs out. */
static cJSON *places_call(const char *state, const char *cache) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    return built_call(call, places != NULL && add_text(places, "state", state) && add_text(places, "cache", cache));
}

static int is_command(const runtime_hosts_command *command) {
    return runtime_is_text(command->name) && runtime_is_text(command->target) &&
           runtime_are_arguments(command->arguments, command->argument_count);
}

static int are_commands(const runtime_hosts_command *commands, size_t count) {
    if (count > 0 && commands == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_command(&commands[index])) {
            return 0;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (strcmp(commands[earlier].name, commands[index].name) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static spisor_status check_register(const runtime_hosts_register_argument *argument, spisor_refusal *refusal) {
    const runtime_registration *registration = argument->registration;
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    if (!runtime_is_place(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    if (!are_commands(argument->commands, argument->command_count)) {
        return runtime_refuse_member(refusal, "commands", HEADER);
    }
    if (registration != NULL &&
        spisor_has_shape(registration->size, SPISOR_SHAPE_THROUGH(runtime_registration, start))) {
        return runtime_check_host_id(refusal, registration->id, HEADER);
    }
    return SPISOR_OK;
}

static cJSON *command_json(const runtime_hosts_command *command) {
    cJSON *json = cJSON_CreateObject();
    return built_call(json, json != NULL && add_text(json, "name", command->name) &&
                                add_text(json, "target", command->target) &&
                                add_texts(json, "arguments", command->arguments, command->argument_count));
}

static int add_commands(cJSON *identity, const runtime_hosts_register_argument *argument) {
    cJSON *commands = cJSON_AddArrayToObject(identity, "commands");
    int built = commands != NULL;
    for (size_t index = 0; built && index < argument->command_count; index++) {
        cJSON *command = command_json(&argument->commands[index]);
        built = command != NULL && cJSON_AddItemToArray(commands, command);
        if (!built) {
            cJSON_Delete(command);
        }
    }
    return built;
}

/** {places: {state}, identity, provenance, environment}, `*call` NULL unless the answer is OK. */
static spisor_status register_call(const runtime_hosts_register_argument *argument, cJSON **call,
                                   spisor_refusal *refusal) {
    cJSON *identity = NULL;
    spisor_status status = runtime_registration_record_json(argument->registration, &identity, refusal);
    cJSON *provenance;
    int built;
    *call = NULL;
    if (status != SPISOR_OK) {
        return status;
    }
    provenance = cJSON_DetachItemFromObjectCaseSensitive(identity, "provenance");
    *call = places_call(argument->state, NULL);
    built = *call != NULL && add_commands(identity, argument) &&
            add_environment(*call, argument->environment, argument->environment_count);
    built = runtime_add_owned(built ? *call : NULL, "provenance", provenance) && built;
    built = runtime_add_owned(built ? *call : NULL, "identity", identity) && built;
    if (!built) {
        cJSON_Delete(*call);
        *call = NULL;
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status register_host(void *context, const runtime_hosts_register_argument *argument,
                                   runtime_install_register_loader_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, REGISTER_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, REGISTER_ANSWER_SHAPE,
                                         answer->command_count | answer->refused_command_count)) {
        return spisor_refuse_shape(refusal, "hosts' register_host");
    }
    status = check_register(argument, refusal);
    if (status == SPISOR_OK) {
        status = register_call(argument, &call, refusal);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    runtime_hosts_registration_request request = {argument->state, cJSON_GetObjectItemCaseSensitive(call, "identity"),
                                                  cJSON_GetObjectItemCaseSensitive(call, "provenance"),
                                                  cJSON_GetObjectItemCaseSensitive(call, "environment")};
    answered = runtime_hosts_register(&request, &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, runtime_fill_register_answer, answer,
                                  REGISTER_ANSWER_SHAPE, refusal);
}

static spisor_status list(void *context, const runtime_registry_list_argument *argument,
                          runtime_registry_listing *answer, spisor_refusal *refusal) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    (void)context;
    return registry->list(registry->context, argument, answer, refusal);
}

/** Fills the answer from the reversal before the forget, so the forget is followed by no allocation. */
static int fill_before_forget(const cJSON *reversed, void *filled) {
    runtime_hosts_remove_answer *answer = filled;
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(reversed, "reversed");
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = runtime_fill_drifted(&answer->storage, cJSON_GetObjectItemCaseSensitive(reversed, "drifted"),
                                      &answer->drifted, &answer->drifted_count);
    }
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, REMOVE_ANSWER_SHAPE);
        return 0;
    }
    answer->reversed = cJSON_IsNumber(count) ? (uint64_t)count->valuedouble : 0;
    return 1;
}

static spisor_status check_remove(const runtime_registry_forget_argument *argument, spisor_refusal *refusal) {
    if (!runtime_is_place(argument->state) || !runtime_is_place(argument->cache)) {
        return runtime_refuse_place(refusal, !runtime_is_place(argument->state) ? "state" : "cache");
    }
    return runtime_check_host_id(refusal, argument->id, HEADER);
}

static spisor_status remove_host(void *context, const runtime_registry_forget_argument *argument,
                                 runtime_hosts_remove_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *removed;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, REMOVE_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, REMOVE_ANSWER_SHAPE, answer->drifted_count)) {
        return spisor_refuse_shape(refusal, "hosts' remove");
    }
    status = check_remove(argument, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    removed = runtime_hosts_remove(argument->state, argument->cache, argument->id, fill_before_forget, answer,
                                   &operation_refusal);
    if (removed == NULL) {
        runtime_release_stored_answer(answer, REMOVE_ANSWER_SHAPE);
        return runtime_refuse_json(refusal, operation_refusal);
    }
    answer->forgotten = flag_of(removed, "forgotten");
    cJSON_Delete(removed);
    return SPISOR_OK;
}

static int is_writable_time(double now) {
    char text[RUNTIME_CATALOG_TIME_SIZE];
    return isfinite(now) && now >= 0 && runtime_catalog_format_time(now, text);
}

static spisor_status check_provenance(const runtime_hosts_provenance_argument *argument, spisor_refusal *refusal) {
    const runtime_install_origin *origin = argument->origin;
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    if (origin == NULL || !runtime_is_text(origin->package.method) ||
        (origin->package.name != NULL && !runtime_is_text(origin->package.name)) ||
        (origin->version != NULL && !runtime_is_text(origin->version))) {
        return runtime_refuse_member(refusal, "origin", HEADER);
    }
    if (!runtime_is_text(argument->binary)) {
        return runtime_refuse_member(refusal, "binary", HEADER);
    }
    if (!runtime_is_place(argument->home)) {
        return runtime_refuse_place(refusal, "home");
    }
    return is_writable_time(argument->now) ? SPISOR_OK : runtime_refuse_member(refusal, "now", HEADER);
}

static cJSON *origin_json(const runtime_install_origin *origin, spisor_status *status) {
    cJSON *json = cJSON_CreateObject();
    cJSON *options = NULL;
    int built;
    *status = origin->package.options == NULL ? SPISOR_OK : spisor_json_of_value(origin->package.options, &options);
    built = *status == SPISOR_OK && json != NULL && add_text(json, "method", origin->package.method) &&
            add_text(json, "package", origin->package.name) && add_text(json, "version", origin->version);
    if (options != NULL) {
        built = runtime_add_owned(built ? json : NULL, "options", options) && built;
    }
    if (*status == SPISOR_OK && !built) {
        *status = SPISOR_OUT_OF_MEMORY;
    }
    return built_call(json, *status == SPISOR_OK);
}

static spisor_status provenance_call(const runtime_hosts_provenance_argument *argument, cJSON **call) {
    spisor_status status;
    cJSON *origin = origin_json(argument->origin, &status);
    cJSON *application;
    int built;
    if (status != SPISOR_OK) {
        *call = NULL;
        return status;
    }
    *call = cJSON_CreateObject();
    application = *call == NULL ? NULL : cJSON_AddObjectToObject(*call, "application");
    built = runtime_add_owned(application != NULL ? *call : NULL, "origin", origin) &&
            add_text(application, "binary", argument->binary) && add_text(*call, "home", argument->home) &&
            add_environment(*call, argument->environment, argument->environment_count) &&
            (argument->now == 0 || cJSON_AddNumberToObject(*call, "now", argument->now) != NULL);
    *call = built_call(*call, built);
    return built ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_provenance(const cJSON *answered, void *filled) {
    runtime_hosts_provenance_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? runtime_storage_provenance(&answer->storage, answered, &answer->provenance) : status;
}

static spisor_status provenance(void *context, const runtime_hosts_provenance_argument *argument,
                                runtime_hosts_provenance_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, PROVENANCE_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, PROVENANCE_ANSWER_SHAPE)) {
        return spisor_refuse_shape(refusal, "hosts' provenance");
    }
    status = check_provenance(argument, refusal);
    if (status == SPISOR_OK) {
        status = provenance_call(argument, &call);
    }
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_member(refusal, "origin options", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    runtime_hosts_provenance_request request = {
        cJSON_GetObjectItemCaseSensitive(call, "origin"), argument->binary, argument->home,
        cJSON_GetObjectItemCaseSensitive(call, "environment"), argument->now != 0, argument->now};
    answered = runtime_hosts_provenance(&request, &operation_refusal);
    cJSON_Delete(call);
    return runtime_serve_answered(answered, operation_refusal, fill_provenance, answer, PROVENANCE_ANSWER_SHAPE,
                                  refusal);
}

static const char *layer_runtime_of(const runtime_hosts_start_argument *argument) {
    return spisor_has_shape(argument->size, SPISOR_SHAPE_THROUGH(runtime_hosts_start_argument, layer_runtime))
               ? argument->layer_runtime
               : NULL;
}

static const char *action_plugin_of(const runtime_hosts_start_argument *argument) {
    return spisor_has_shape(argument->size, SPISOR_SHAPE_THROUGH(runtime_hosts_start_argument, action_plugin))
               ? argument->action_plugin
               : NULL;
}

static spisor_status check_start(const runtime_hosts_start_argument *argument, spisor_refusal *refusal) {
    spisor_status status = runtime_is_environment(argument->environment, argument->environment_count)
                               ? runtime_check_host_id(refusal, argument->host_id, HEADER)
                               : runtime_refuse_environment(refusal);
    if (status == SPISOR_OK && (argument->layer == NULL || !runtime_are_item_versions(argument->layer, 1))) {
        status = runtime_refuse_member(refusal, "layer", HEADER);
    }
    if (status == SPISOR_OK && !runtime_are_item_versions(argument->libraries, argument->library_count)) {
        status = runtime_refuse_member(refusal, "libraries", HEADER);
    }
    if (status == SPISOR_OK && argument->session > 1) {
        status = runtime_refuse_member(refusal, "session", HEADER);
    }
    if (status == SPISOR_OK && !runtime_is_layer_runtime(layer_runtime_of(argument))) {
        status = runtime_refuse_member(refusal, "layer_runtime", HEADER);
    }
    return status;
}

/** {environment, libraries}, the two objects hosts.start reads; NULL when memory runs out. */
static cJSON *start_call(const runtime_hosts_start_argument *argument) {
    cJSON *call = cJSON_CreateObject();
    cJSON *libraries = call == NULL ? NULL : cJSON_AddObjectToObject(call, "libraries");
    int built = libraries != NULL && add_environment(call, argument->environment, argument->environment_count);
    for (size_t index = 0; built && index < argument->library_count; index++) {
        built = add_text(libraries, argument->libraries[index].id, argument->libraries[index].version);
    }
    return built_call(call, built);
}

static spisor_status fill_places(void **storage, const cJSON *json, const runtime_places_answer **out) {
    runtime_places_answer *places = runtime_storage_take(storage, sizeof *places);
    if (places == NULL || !runtime_storage_copy(storage, text_of(json, "home"), &places->home) ||
        !runtime_storage_copy(storage, text_of(json, "portable"), &places->portable) ||
        !runtime_storage_copy(storage, text_of(json, "state"), &places->state) ||
        !runtime_storage_copy(storage, text_of(json, "cache"), &places->cache) ||
        !runtime_storage_copy(storage, text_of(json, "secretsFile"), &places->secrets_file)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    places->size = sizeof *places;
    places->force_file_store = flag_of(json, "forceFileStore");
    *out = places;
    return SPISOR_OK;
}

static spisor_status fill_plan(void **storage, const cJSON *json, const runtime_updates_apply_answer **out) {
    runtime_updates_apply_answer *plan = runtime_storage_take(storage, sizeof *plan);
    spisor_status status = plan == NULL ? SPISOR_OUT_OF_MEMORY : runtime_fill_apply_plan(storage, json, plan);
    if (status == SPISOR_OK) {
        plan->size = sizeof *plan;
        *out = plan;
    }
    return status;
}

static spisor_status fill_timers(void **storage, const cJSON *json, const runtime_hosts_timers **out) {
    runtime_hosts_timers *timers;
    if (json == NULL) {
        return SPISOR_OK;
    }
    timers = runtime_storage_take(storage, sizeof *timers);
    if (timers == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    timers->record_run_milliseconds =
        cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "recordRunMilliseconds"));
    timers->prune_failures_before_exit =
        (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "pruneFailuresBeforeExit"));
    timers->update_check_milliseconds =
        cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(json, "updateCheckMilliseconds"));
    *out = timers;
    return SPISOR_OK;
}

static spisor_status fill_identity(void **storage, const cJSON *identity, runtime_hosts_start_answer *answer) {
    if (!runtime_storage_copy(storage, text_of(identity, "version"), &answer->version) ||
        !runtime_storage_copy(storage, text_of(identity, "home"), &answer->home)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->requires_command = flag_of(identity, "requiresCommand");
    return runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(identity, "application"),
                                 &answer->application);
}

static spisor_status fill_start(const cJSON *answered, runtime_hosts_start_answer *answer) {
    void **storage = &answer->storage;
    spisor_status status = runtime_start_storage(storage);
    if (status == SPISOR_OK) {
        status = fill_places(storage, cJSON_GetObjectItemCaseSensitive(answered, "places"), &answer->places);
    }
    if (status == SPISOR_OK) {
        status = fill_identity(storage, cJSON_GetObjectItemCaseSensitive(answered, "identity"), answer);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(answered, "settings"),
                                       &answer->settings);
    }
    if (status == SPISOR_OK) {
        status = fill_plan(storage, cJSON_GetObjectItemCaseSensitive(answered, "plan"), &answer->plan);
    }
    if (status == SPISOR_OK) {
        status = fill_timers(storage, cJSON_GetObjectItemCaseSensitive(answered, "timers"), &answer->timers);
    }
    answer->reused_recorded_run = flag_of(answered, "reusedRecordedRun");
    answer->no_color = flag_of(answered, "noColor");
    return status;
}

/** Starts the host, and when the answer cannot be filled removes the run record the start wrote. */
static spisor_status serve_start(const runtime_hosts_start_argument *argument, runtime_hosts_start_answer *answer,
                                 spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *call = start_call(argument);
    runtime_hosts_start_request request = {cJSON_GetObjectItemCaseSensitive(call, "environment"),
                                           argument->host_id,
                                           argument->layer->id,
                                           argument->layer->version,
                                           cJSON_GetObjectItemCaseSensitive(call, "libraries"),
                                           argument->session != 0,
                                           0,
                                           layer_runtime_of(argument),
                                           action_plugin_of(argument)};
    cJSON *answered = call == NULL ? NULL : runtime_hosts_start(&request, &operation_refusal);
    spisor_status status;
    cJSON_Delete(call);
    if (answered == NULL) {
        return runtime_refuse_json(refusal, operation_refusal);
    }
    status = fill_start(answered, answer);
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, START_ANSWER_SHAPE);
        runtime_hosts_start_undo(cJSON_GetObjectItemCaseSensitive(answered, "plan"));
        status = SPISOR_OUT_OF_MEMORY;
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status start(void *context, const runtime_hosts_start_argument *argument,
                           runtime_hosts_start_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, START_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, START_ANSWER_SHAPE)) {
        return spisor_refuse_shape(refusal, "hosts' start");
    }
    status = check_start(argument, refusal);
    return status != SPISOR_OK ? status : serve_start(argument, answer, refusal);
}

static const char *result_of(const runtime_hosts_layer_start_argument *argument) {
    return spisor_has_shape(argument->size, LAYER_START_RESULT_SHAPE) ? argument->result : NULL;
}

/**
 * Whether the plugin, action and input are all given texts for an ACTION, its result a text when given, and all four
 * NULL for any other mode.
 */
static int are_action_members(const runtime_hosts_layer_start_argument *argument) {
    const char *result = result_of(argument);
    if (argument->mode == RUNTIME_HOSTS_LAYER_ACTION) {
        return runtime_is_text(argument->plugin) && runtime_is_text(argument->action) &&
               runtime_is_text(argument->input) && (result == NULL || runtime_is_text(result));
    }
    return argument->plugin == NULL && argument->action == NULL && argument->input == NULL && result == NULL;
}

static spisor_status check_layer_start(const runtime_hosts_layer_start_argument *argument, spisor_refusal *refusal) {
    spisor_status status = runtime_is_environment(argument->environment, argument->environment_count)
                               ? runtime_check_host_id(refusal, argument->loader_id, HEADER)
                               : runtime_refuse_environment(refusal);
    if (status == SPISOR_OK &&
        (argument->mode < RUNTIME_HOSTS_LAYER_SERVE || argument->mode > RUNTIME_HOSTS_LAYER_ACTION)) {
        status = runtime_refuse_member(refusal, "mode", HEADER);
    }
    if (status == SPISOR_OK && !are_action_members(argument)) {
        status = runtime_refuse_member(refusal, "plugin, action, input and result", HEADER);
    }
    return status;
}

static spisor_status fill_variables(void **storage, const cJSON *environment, runtime_hosts_layer *layer) {
    runtime_variable *variables;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(environment), sizeof *variables,
                                    (void **)&variables)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    layer->environment = variables;
    cJSON_ArrayForEach(member, environment) {
        runtime_variable *each = &variables[layer->environment_count++];
        if (!runtime_storage_copy(storage, member->string, &each->name) ||
            !runtime_storage_copy(storage, cJSON_GetStringValue(member), &each->value)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_layer(void **storage, const cJSON *json, runtime_hosts_layer *layer) {
    if (!runtime_storage_copy(storage, text_of(json, "runtime"), &layer->runtime) ||
        !runtime_storage_copy(storage, text_of(json, "hostId"), &layer->host_id) ||
        !runtime_storage_copy(storage, text_of(json, "executable"), &layer->executable) ||
        !runtime_storage_texts(storage, cJSON_GetObjectItemCaseSensitive(json, "arguments"), &layer->arguments,
                               &layer->argument_count)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return fill_variables(storage, cJSON_GetObjectItemCaseSensitive(json, "environment"), layer);
}

static spisor_status fill_layer_start(const cJSON *answered, void *filled) {
    runtime_hosts_layer_start_answer *answer = filled;
    const cJSON *layers = cJSON_GetObjectItemCaseSensitive(answered, "layers");
    const cJSON *layer;
    runtime_hosts_layer *each;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(layers),
                                                           sizeof *each, (void **)&each)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->layers = each;
    cJSON_ArrayForEach(layer, layers) {
        status = status == SPISOR_OK ? fill_layer(&answer->storage, layer, &each[answer->layer_count++]) : status;
    }
    return status;
}

static spisor_status layer_start(void *context, const runtime_hosts_layer_start_argument *argument,
                                 runtime_hosts_layer_start_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *environment = NULL;
    cJSON *answered;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, LAYER_START_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, LAYER_START_ANSWER_SHAPE, answer->layer_count)) {
        return spisor_refuse_shape(refusal, "hosts' layer_start");
    }
    status = check_layer_start(argument, refusal);
    if (status == SPISOR_OK) {
        status = runtime_environment_json(argument->environment, argument->environment_count, &environment);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    runtime_hosts_layer_start_request request = {environment,      argument->loader_id, argument->mode,
                                                 argument->plugin, argument->action,    argument->input,
                                                 result_of(argument)};
    answered = runtime_hosts_layer_start(&request, &operation_refusal);
    cJSON_Delete(environment);
    return runtime_serve_answered(answered, operation_refusal, fill_layer_start, answer, LAYER_START_ANSWER_SHAPE,
                                  refusal);
}

static void release_register_answer(void *context, runtime_install_register_loader_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REGISTER_ANSWER_SHAPE);
}

static void release_listing(void *context, runtime_registry_listing *answer) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    (void)context;
    registry->release_listing(registry->context, answer);
}

static void release_remove_answer(void *context, runtime_hosts_remove_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REMOVE_ANSWER_SHAPE);
}

static void release_provenance_answer(void *context, runtime_hosts_provenance_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PROVENANCE_ANSWER_SHAPE);
}

static void release_start_answer(void *context, runtime_hosts_start_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, START_ANSWER_SHAPE);
}

static void release_layer_start_answer(void *context, runtime_hosts_layer_start_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LAYER_START_ANSWER_SHAPE);
}

static const runtime_hosts_definition definition = {
    sizeof(runtime_hosts_definition),
    NULL,
    register_host,
    list,
    remove_host,
    provenance,
    start,
    release_register_answer,
    release_listing,
    release_remove_answer,
    release_provenance_answer,
    release_start_answer,
    spisor_release_refusal,
    layer_start,
    release_layer_start_answer,
};

RUNTIME_DEFINITION_API const void *runtime_hosts_definition_of(void) {
    return &definition;
}
