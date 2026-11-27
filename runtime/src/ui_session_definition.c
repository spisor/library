#include "runtime/ui.h"

#include <string.h>

#include "allocation.h"
#include "answer_storage.h"
#include "catalog_source.h"
#include "config_conversion.h"
#include "definition_support.h"
#include "install_run.h"
#include "ui_definition.h"
#include "ui_session.h"

#define SESSION_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_session_argument, loader)
#define SESSION_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_ui_session_answer, storage)
#define PLACES_SHAPE SPISOR_SHAPE_THROUGH(runtime_places_answer, storage)

/** What every plugin's config shares: the host's home and portable place and where its secrets live. */
typedef struct session_host {
    const char *home;
    const char *portable;
    const runtime_secret_places *places;
} session_host;

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static const char *value_text(const spisor_value *record, const char *key) {
    const spisor_value *member = spisor_value_member(record, key, strlen(key));
    return member != NULL && member->kind == SPISOR_VALUE_TEXT ? member->text : NULL;
}

/** The declarations of a description's settings, which its scope's read has checked; 0 when memory runs out. */
static int fill_declarations(void **storage, const spisor_value *description, runtime_config_plugin *config) {
    const spisor_value *settings = spisor_value_member(description, "settings", 8);
    runtime_setting_declaration *declarations = NULL;
    size_t count = settings != NULL && settings->kind == SPISOR_VALUE_RECORD ? settings->count : 0;
    if (!runtime_storage_take_array(storage, count, sizeof *declarations, (void **)&declarations)) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        const spisor_entry *entry = &settings->entries[index];
        declarations[index].key = entry->key;
        declarations[index].type = value_text(&entry->value, "type");
        declarations[index].layer = runtime_settings_layer_named(value_text(&entry->value, "layer"));
    }
    config->settings = declarations;
    config->setting_count = count;
    return 1;
}

static spisor_status fill_plugin(void **storage, const cJSON *json, const spisor_value *description,
                                 const session_host *host, runtime_ui_session_plugin *plugin) {
    runtime_config_plugin *config = runtime_storage_take(storage, sizeof *config);
    if (config == NULL || !fill_declarations(storage, description, config) ||
        !runtime_storage_copy(storage, text_of(json, "id"), &plugin->id) ||
        !runtime_storage_copy(storage, text_of(json, "version"), &plugin->version)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    config->size = (uint32_t)sizeof *config;
    config->home = host->home;
    config->portable = host->portable;
    config->plugin = value_text(description, "id");
    config->config = spisor_value_member(description, "config", 6);
    config->places = host->places;
    plugin->description = description;
    plugin->config = config;
    return SPISOR_OK;
}

/** The home, portable place and secret places every plugin's config points at, in the storage. */
static spisor_status fill_host(void **storage, const cJSON *answered, session_host *host) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(answered, "places");
    runtime_secret_places *secrets = runtime_storage_take(storage, sizeof *secrets);
    if (secrets == NULL || !runtime_storage_copy(storage, text_of(answered, "home"), &host->home) ||
        !runtime_storage_copy(storage, text_of(places, "portable"), &host->portable) ||
        !runtime_storage_copy(storage, text_of(places, "secretsFile"), &secrets->secrets_file)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    secrets->size = (uint32_t)sizeof *secrets;
    secrets->force_file_store = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(places, "forceFileStore")) ? 1u : 0u;
    host->places = secrets;
    return SPISOR_OK;
}

static spisor_status fill_plugins(runtime_ui_session_answer *answer, const cJSON *answered) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(answered, "plugins");
    const cJSON *plugin;
    session_host host;
    runtime_ui_session_plugin *filled = NULL;
    spisor_status status = fill_host(&answer->storage, answered, &host);
    if (status == SPISOR_OK) {
        status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "descriptions"),
                                       &answer->descriptions);
    }
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(plugins),
                                                           sizeof *filled, (void **)&filled)) {
        return status == SPISOR_OK ? SPISOR_OUT_OF_MEMORY : status;
    }
    answer->plugins = filled;
    cJSON_ArrayForEach(plugin, plugins) {
        if (status == SPISOR_OK) {
            status = fill_plugin(&answer->storage, plugin, &answer->descriptions->items[answer->plugin_count], &host,
                                 &filled[answer->plugin_count]);
            answer->plugin_count++;
        }
    }
    return status;
}

static spisor_status fill_sources(runtime_ui_session_answer *answer, const cJSON *sources) {
    runtime_catalog_location *read = NULL;
    runtime_catalog_source *filled = NULL;
    char *problem = NULL;
    size_t count = 0;
    int built = runtime_catalog_sources_of(sources, &read, &count, &problem) &&
                runtime_storage_take_array(&answer->storage, count, sizeof *filled, (void **)&filled);
    for (size_t index = 0; built && index < count; index++) {
        filled[index].kind = read[index].local ? RUNTIME_CATALOG_LOCAL : RUNTIME_CATALOG_HTTPS;
        built = runtime_storage_copy(&answer->storage, read[index].location, &filled[index].location);
    }
    if (built) {
        answer->sources = filled;
        answer->source_count = count;
    }
    spisor_memory_release(problem);
    spisor_memory_release(read);
    return built ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_session(const cJSON *answered, void *filled) {
    runtime_ui_session_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_copy(&answer->storage, text_of(answered, "host"), &answer->host)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = fill_plugins(answer, answered);
    if (status == SPISOR_OK) {
        status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "ui"), &answer->ui);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "installed"),
                                       &answer->installed);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "detected"),
                                       &answer->detected);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "available"),
                                       &answer->available);
    }
    return status == SPISOR_OK ? fill_sources(answer, cJSON_GetObjectItemCaseSensitive(answered, "sources")) : status;
}

static spisor_status check_places(const runtime_places_answer *places, spisor_refusal *refusal) {
    int usable = places != NULL && spisor_has_shape(places->size, PLACES_SHAPE) && runtime_is_place(places->portable) &&
                 runtime_is_place(places->state) && runtime_is_place(places->cache) &&
                 runtime_is_place(places->secrets_file) && places->force_file_store <= 1;
    return usable ? SPISOR_OK : runtime_refuse_member(refusal, "places", RUNTIME_UI_HEADER);
}

/** {portable, state, cache, secretsFile, forceFileStore} of checked places; NULL when memory runs out. */
static cJSON *places_json(const runtime_places_answer *places) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL || cJSON_AddStringToObject(json, "portable", places->portable) == NULL ||
        cJSON_AddStringToObject(json, "state", places->state) == NULL ||
        cJSON_AddStringToObject(json, "cache", places->cache) == NULL ||
        cJSON_AddStringToObject(json, "secretsFile", places->secrets_file) == NULL ||
        cJSON_AddBoolToObject(json, "forceFileStore", places->force_file_store == 1) == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

spisor_status runtime_ui_session_operation(void *context, const runtime_ui_session_argument *argument,
                                           runtime_ui_session_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    cJSON *places;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, SESSION_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, SESSION_ANSWER_SHAPE, answer->plugin_count | answer->source_count)) {
        return spisor_refuse_shape(refusal, "ui's session");
    }
    status = check_places(argument->places, refusal);
    if (status == SPISOR_OK && argument->loader != NULL) {
        status = runtime_check_host_id(refusal, argument->loader, RUNTIME_UI_HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    places = places_json(argument->places);
    answered = places == NULL ? NULL
                              : runtime_ui_session(places, argument->loader == NULL ? RUNTIME_ECOSYSTEM_HOST_ID
                                                                                    : argument->loader,
                                                   &operation_refusal);
    cJSON_Delete(places);
    return runtime_serve_answered(answered, operation_refusal, fill_session, answer, SESSION_ANSWER_SHAPE, refusal);
}

void runtime_ui_release_session_answer(void *context, runtime_ui_session_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SESSION_ANSWER_SHAPE);
}
