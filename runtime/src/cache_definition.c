#include "runtime/cache.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "cache.h"
#include "cache_deploy.h"
#include "definition_support.h"
#include "identity.h"
#include "json_service.h"
#include "json_value.h"
#include "platform_key.h"

#define INSTALL_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_install_argument, platform)
#define INSTALL_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_install_answer, storage)
#define PLACE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_place_argument, platform)
#define INSTALLED_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_installed_answer, storage)
#define CHECK_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_check_argument, directory)
#define CHECK_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_check_answer, storage)
#define DAMAGED_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_damaged_answer, storage)
#define LOCATE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_locate_argument, plugin_count)
#define LOCATE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_locate_answer, storage)
#define PLATFORM_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_platform_answer, storage)
#define SWEEP_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_sweep_argument, state)
#define SWEEP_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_sweep_answer, storage)
#define DEPLOY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_deploy_argument, replaces)
#define DEPLOY_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_cache_deploy_answer, storage)

static int is_text(const char *text) {
    return text != NULL && text[0] != '\0';
}

static spisor_status refuse_missing(spisor_refusal *refusal, const char *member) {
    char detail[160];
    char fix[160];
    snprintf(detail, sizeof detail, "the argument carries no %s", member);
    snprintf(fix, sizeof fix, "send %s as text that is not empty", member);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static int add_text(cJSON *call, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(call, name, text) != NULL;
}

/** A call of up to three named texts, a NULL text left out; NULL when memory runs out. */
static cJSON *call_of(const char *const *names, const char *const *texts, size_t count) {
    cJSON *call = cJSON_CreateObject();
    for (size_t index = 0; call != NULL && index < count; index++) {
        if (!add_text(call, names[index], texts[index])) {
            cJSON_Delete(call);
            call = NULL;
        }
    }
    return call;
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static spisor_status copy_texts(void **storage, const cJSON *array, const char *const **texts, size_t *count) {
    return runtime_storage_texts(storage, array, texts, count) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status copy_members(void **storage, const cJSON *object, const char *const *names, const char **copies,
                                  size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!runtime_storage_copy(storage, text_of(object, names[index]), &copies[index])) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_install(const cJSON *answered, void *filled) {
    static const char *const names[3] = {"id", "version", "directory"};
    runtime_cache_install_answer *answer = filled;
    const char *copies[3] = {NULL, NULL, NULL};
    spisor_status status = runtime_storage_take(&answer->storage, 1) == NULL
                               ? SPISOR_OUT_OF_MEMORY
                               : copy_members(&answer->storage, answered, names, copies, 3);
    answer->id = copies[0];
    answer->version = copies[1];
    answer->directory = copies[2];
    if (status == SPISOR_OK) {
        status = copy_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "fetched"), &answer->fetched,
                            &answer->fetched_count);
    }
    if (status == SPISOR_OK) {
        status = copy_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "cached"), &answer->cached,
                            &answer->cached_count);
    }
    return status;
}

/** Stores the version and takes the answer's storage before the completion marker is written. */
static spisor_status serve_install(cJSON *call, runtime_cache_install_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    runtime_cache_installation *installation = runtime_cache_install_storing(call, &operation_refusal);
    spisor_status status = installation == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                                : fill_install(runtime_cache_installation_answer(installation), answer);
    if (status == SPISOR_OK && !runtime_cache_mark_installation(installation, &operation_refusal)) {
        status = runtime_refuse_json(refusal, operation_refusal);
    }
    if (installation != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, INSTALL_ANSWER_SHAPE);
    }
    runtime_cache_free_installation(installation);
    cJSON_Delete(call);
    return status;
}

/** The item as the JSON an install reads; MALFORMED for one that is not a well-formed record. */
static spisor_status item_json(const spisor_value *item, cJSON **json) {
    spisor_status status = item == NULL ? SPISOR_MALFORMED : spisor_value_check(item);
    if (status == SPISOR_OK && item->kind != SPISOR_VALUE_RECORD) {
        status = SPISOR_MALFORMED;
    }
    return status == SPISOR_OK ? spisor_json_of_value(item, json) : status;
}

static spisor_status install(void *context, const runtime_cache_install_argument *argument,
                             runtime_cache_install_answer *answer, spisor_refusal *refusal) {
    const char *names[4] = {"cache", "id", "version", "platform"};
    const char *texts[4];
    cJSON *item = NULL;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, INSTALL_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, INSTALL_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's install");
    }
    if (!is_text(argument->cache) || !is_text(argument->id) || !is_text(argument->version) ||
        (argument->platform != NULL && !is_text(argument->platform))) {
        return refuse_missing(refusal, !is_text(argument->cache)     ? "cache"
                                       : !is_text(argument->id)      ? "id"
                                       : !is_text(argument->version) ? "version"
                                                                     : "platform");
    }
    status = item_json(argument->item, &item);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the item is not a well-formed record",
                                 "send the catalog item as catalog's read answered it");
    }
    if (status != SPISOR_OK) {
        return status;
    }
    texts[0] = argument->cache;
    texts[1] = argument->id;
    texts[2] = argument->version;
    texts[3] = argument->platform;
    call = call_of(names, texts, 4);
    if (call == NULL || !cJSON_AddItemToObject(call, "item", item)) {
        cJSON_Delete(item);
        cJSON_Delete(call);
        return SPISOR_OUT_OF_MEMORY;
    }
    return serve_install(call, answer, refusal);
}

/** An {id: [version]} object as an array of versions, in its order. */
static spisor_status fill_versions(void **storage, const cJSON *object, const runtime_cache_versions **filled,
                                   size_t *count) {
    runtime_cache_versions *versions;
    const cJSON *member;
    size_t index = 0;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *versions,
                                    (void **)&versions)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(member, object) {
        runtime_cache_versions *each = &versions[index++];
        if (!runtime_storage_copy(storage, member->string, &each->id) ||
            copy_texts(storage, member, &each->versions, &each->version_count) != SPISOR_OK) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    *filled = versions;
    *count = index;
    return SPISOR_OK;
}

static spisor_status fill_installed(const cJSON *answered, void *filled) {
    runtime_cache_installed_answer *answer = filled;
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(answered, "bundles");
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(answered, "libraries");
    spisor_status status;
    if (runtime_storage_take(&answer->storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = fill_versions(&answer->storage, bundles, &answer->bundles, &answer->bundle_count);
    return status != SPISOR_OK
               ? status
               : fill_versions(&answer->storage, libraries, &answer->libraries, &answer->library_count);
}

static spisor_status place_call(const runtime_cache_place_argument *argument, cJSON **call, spisor_refusal *refusal) {
    const char *names[2] = {"cache", "platform"};
    const char *texts[2];
    if (!is_text(argument->cache)) {
        return refuse_missing(refusal, "cache");
    }
    texts[0] = argument->cache;
    texts[1] = argument->platform;
    *call = call_of(names, texts, 2);
    return *call == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status installed(void *context, const runtime_cache_place_argument *argument,
                               runtime_cache_installed_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PLACE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, INSTALLED_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's installed");
    }
    status = place_call(argument, &call, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_cache_installed_operation, call, fill_installed, answer,
                                       INSTALLED_ANSWER_SHAPE, refusal);
}

static uint32_t verdict_of(const char *kind) {
    if (kind != NULL && strcmp(kind, "intact") == 0) {
        return RUNTIME_CACHE_INTACT;
    }
    return kind != NULL && strcmp(kind, "damaged") == 0 ? RUNTIME_CACHE_DAMAGED : RUNTIME_CACHE_ABSENT;
}

static spisor_status fill_check(const cJSON *answered, void *filled) {
    static const char *const names[3] = {"detail", "fix", "file"};
    runtime_cache_check_answer *answer = filled;
    const char *copies[3] = {NULL, NULL, NULL};
    spisor_status status;
    if (runtime_storage_take(&answer->storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = copy_members(&answer->storage, answered, names, copies, 3);
    answer->verdict = verdict_of(text_of(answered, "kind"));
    answer->detail = copies[0];
    answer->fix = copies[1];
    answer->file = copies[2];
    return status;
}

static spisor_status check(void *context, const runtime_cache_check_argument *argument,
                           runtime_cache_check_answer *answer, spisor_refusal *refusal) {
    const char *name = "directory";
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CHECK_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, CHECK_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's check");
    }
    if (!is_text(argument->directory)) {
        return refuse_missing(refusal, "directory");
    }
    call = call_of(&name, &argument->directory, 1);
    return call == NULL ? SPISOR_OUT_OF_MEMORY
                        : runtime_serve_operation(runtime_cache_check_operation, call, fill_check, answer, CHECK_ANSWER_SHAPE, refusal);
}

/** A JSON array of {id, version, detail, fix, file?} as damages, in order. */
static spisor_status fill_damages(void **storage, const cJSON *array, const runtime_cache_damage **filled,
                                  size_t *count) {
    static const char *const names[5] = {"id", "version", "detail", "fix", "file"};
    runtime_cache_damage *damages;
    const cJSON *entry;
    size_t index = 0;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), sizeof *damages, (void **)&damages)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, array) {
        const char *copies[5] = {NULL, NULL, NULL, NULL, NULL};
        runtime_cache_damage *each = &damages[index++];
        spisor_status status = copy_members(storage, entry, names, copies, 5);
        each->id = copies[0];
        each->version = copies[1];
        each->detail = copies[2];
        each->fix = copies[3];
        each->file = copies[4];
        if (status != SPISOR_OK) {
            return status;
        }
    }
    *filled = damages;
    *count = index;
    return SPISOR_OK;
}

static spisor_status fill_damaged(const cJSON *answered, void *filled) {
    runtime_cache_damaged_answer *answer = filled;
    if (runtime_storage_take(&answer->storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return fill_damages(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "damaged"), &answer->damaged,
                        &answer->damaged_count);
}

static spisor_status damaged(void *context, const runtime_cache_place_argument *argument,
                             runtime_cache_damaged_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PLACE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, DAMAGED_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's damaged");
    }
    status = place_call(argument, &call, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_cache_damaged_operation, call, fill_damaged, answer,
                                       DAMAGED_ANSWER_SHAPE, refusal);
}

static spisor_status fill_located(void **storage, const cJSON *array, const runtime_cache_located **filled,
                                  size_t *count) {
    static const char *const names[5] = {"id", "version", "directory", "manifest", "entry"};
    runtime_cache_located *located;
    const cJSON *entry;
    size_t index = 0;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), sizeof *located, (void **)&located)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, array) {
        const char *copies[5] = {NULL, NULL, NULL, NULL, NULL};
        runtime_cache_located *each = &located[index++];
        spisor_status status = copy_members(storage, entry, names, copies, 5);
        each->id = copies[0];
        each->version = copies[1];
        each->directory = copies[2];
        each->manifest = copies[3];
        each->entry = copies[4];
        if (status != SPISOR_OK) {
            return status;
        }
    }
    *filled = located;
    *count = index;
    return SPISOR_OK;
}

static spisor_status fill_locate(const cJSON *answered, void *filled) {
    runtime_cache_locate_answer *answer = filled;
    spisor_status status;
    if (runtime_storage_take(&answer->storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = fill_located(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "located"), &answer->located,
                          &answer->located_count);
    return status != SPISOR_OK ? status
                               : fill_damages(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "problems"),
                                              &answer->problems, &answer->problem_count);
}

/** Whether every plugin has an id and a version and no id is given twice. */
static int are_plugins(const runtime_cache_locate_argument *argument) {
    if (argument->plugin_count > 0 && argument->plugins == NULL) {
        return 0;
    }
    for (size_t index = 0; index < argument->plugin_count; index++) {
        const runtime_cache_plugin *plugin = &argument->plugins[index];
        if (!is_text(plugin->id) || !is_text(plugin->version)) {
            return 0;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (strcmp(argument->plugins[earlier].id, plugin->id) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

/** The plugins as the {id: version} object locate reads, from plugins are_plugins accepted. */
static spisor_status plugins_json(const runtime_cache_locate_argument *argument, cJSON *call) {
    cJSON *plugins = cJSON_AddObjectToObject(call, "plugins");
    for (size_t index = 0; plugins != NULL && index < argument->plugin_count; index++) {
        const runtime_cache_plugin *plugin = &argument->plugins[index];
        if (cJSON_AddStringToObject(plugins, plugin->id, plugin->version) == NULL) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return plugins == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status locate(void *context, const runtime_cache_locate_argument *argument,
                            runtime_cache_locate_answer *answer, spisor_refusal *refusal) {
    const char *name = "cache";
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, LOCATE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, LOCATE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's locate");
    }
    if (!is_text(argument->cache)) {
        return refuse_missing(refusal, "cache");
    }
    if (!are_plugins(argument)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "a plugin lacks its id or its version, or is twice",
                                 "send each plugin once, with its id and the version it runs as text");
    }
    call = call_of(&name, &argument->cache, 1);
    status = call == NULL ? SPISOR_OUT_OF_MEMORY : plugins_json(argument, call);
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status;
    }
    return runtime_serve_operation(runtime_cache_locate_operation, call, fill_locate, answer, LOCATE_ANSWER_SHAPE, refusal);
}

static spisor_status platform(void *context, runtime_cache_platform_answer *answer, spisor_refusal *refusal) {
    const char *key = runtime_platform_key();
    (void)context;
    if (!RUNTIME_IS_EMPTY_ANSWER(answer, PLATFORM_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's platform");
    }
    return runtime_pack_texts(&key, &answer->platform, 1, &answer->storage) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_sweep(const cJSON *answered, void *filled) {
    runtime_cache_sweep_answer *answer = filled;
    if (runtime_storage_take(&answer->storage, 1) == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return copy_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "removed"), &answer->removed,
                      &answer->removed_count);
}

static spisor_status sweep(void *context, const runtime_cache_sweep_argument *argument,
                           runtime_cache_sweep_answer *answer, spisor_refusal *refusal) {
    const char *names[2] = {"cache", "state"};
    const char *texts[2];
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, SWEEP_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, SWEEP_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's sweep");
    }
    if (!is_text(argument->cache) || !is_text(argument->state)) {
        return refuse_missing(refusal, !is_text(argument->cache) ? "cache" : "state");
    }
    texts[0] = argument->cache;
    texts[1] = argument->state;
    call = call_of(names, texts, 2);
    return call == NULL ? SPISOR_OUT_OF_MEMORY
                        : runtime_serve_operation(runtime_cache_sweep_operation, call, fill_sweep, answer, SWEEP_ANSWER_SHAPE, refusal);
}

/** Plans the deploy and takes the answer's storage before carrying it out, so out of memory never follows a write. */
static spisor_status serve_deploy(cJSON *call, runtime_cache_deploy_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    runtime_cache_deployment *plan = runtime_cache_plan_deployment(call, &operation_refusal);
    cJSON_Delete(call);
    if (plan == NULL) {
        return runtime_refuse_json(refusal, operation_refusal);
    }
    const char *import_name = runtime_cache_deployment_import_name(plan);
    spisor_status status =
        runtime_pack_texts(&import_name, &answer->import_name, 1, &answer->storage) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    if (status == SPISOR_OK && !runtime_cache_carry_out_deployment(plan, &operation_refusal)) {
        status = runtime_refuse_json(refusal, operation_refusal);
    }
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, DEPLOY_ANSWER_SHAPE);
    }
    runtime_cache_free_deployment(plan);
    return status;
}

static cJSON *deploy_call(const runtime_cache_deploy_argument *argument) {
    const char *names[3] = {"directory", "pluginDir", "replaces"};
    const char *texts[3];
    cJSON *call;
    cJSON *identity;
    texts[0] = argument->directory;
    texts[1] = argument->plugin_directory;
    texts[2] = argument->replaces;
    call = call_of(names, texts, 3);
    identity = call == NULL ? NULL : cJSON_AddObjectToObject(call, "identity");
    if (identity == NULL || !add_text(identity, "id", argument->id) || !add_text(identity, "name", argument->name)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status deploy(void *context, const runtime_cache_deploy_argument *argument,
                            runtime_cache_deploy_answer *answer, spisor_refusal *refusal) {
    runtime_identity parsed;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, DEPLOY_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, DEPLOY_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "cache's deploy");
    }
    if (!is_text(argument->directory) || !is_text(argument->plugin_directory) || !is_text(argument->name)) {
        return refuse_missing(refusal, !is_text(argument->directory)          ? "directory"
                                       : !is_text(argument->plugin_directory) ? "plugin_directory"
                                                                              : "name");
    }
    if (argument->id == NULL || !runtime_identity_parse_id(argument->id, &parsed)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the identity id is not a valid id",
                                 "send an id such as github:<number> or local:<name>");
    }
    call = deploy_call(argument);
    return call == NULL ? SPISOR_OUT_OF_MEMORY : serve_deploy(call, answer, refusal);
}

static void release_install_answer(void *context, runtime_cache_install_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, INSTALL_ANSWER_SHAPE);
}

static void release_installed_answer(void *context, runtime_cache_installed_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, INSTALLED_ANSWER_SHAPE);
}

static void release_check_answer(void *context, runtime_cache_check_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
}

static void release_damaged_answer(void *context, runtime_cache_damaged_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, DAMAGED_ANSWER_SHAPE);
}

static void release_locate_answer(void *context, runtime_cache_locate_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, LOCATE_ANSWER_SHAPE);
}

static void release_platform_answer(void *context, runtime_cache_platform_answer *answer) {
    (void)context;
    runtime_release_packed_answer(answer, PLATFORM_ANSWER_SHAPE);
}

static void release_sweep_answer(void *context, runtime_cache_sweep_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, SWEEP_ANSWER_SHAPE);
}

static void release_deploy_answer(void *context, runtime_cache_deploy_answer *answer) {
    (void)context;
    runtime_release_packed_answer(answer, DEPLOY_ANSWER_SHAPE);
}

static const runtime_cache_definition definition = {
    sizeof(runtime_cache_definition),
    NULL,
    install,
    installed,
    check,
    damaged,
    locate,
    platform,
    sweep,
    deploy,
    release_install_answer,
    release_installed_answer,
    release_check_answer,
    release_damaged_answer,
    release_locate_answer,
    release_platform_answer,
    release_sweep_answer,
    release_deploy_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_cache_definition_of(void) {
    return &definition;
}
