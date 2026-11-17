#include "runtime/install.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "catalog_listed_source.h"
#include "definition_support.h"
#include "identity.h"
#include "install.h"
#include "install_by_id.h"
#include "install_ecosystem.h"
#include "install_entries.h"
#include "install_methods.h"
#include "install_runtime.h"
#include "install_warnings.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/path.h"

#define WARNINGS_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_warnings_argument, manifest)
#define WARNINGS_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_warnings_answer, storage)
#define PACKAGES_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_packages_argument, manifest_packages)
#define PACKAGES_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_packages_answer, storage)
#define PLAN_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_plan_argument, catalog)
#define PLAN_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_plan_answer, storage)
#define DETECT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_detect_argument, manifest_packages)
#define DETECT_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_detect_answer, storage)
#define ENTRY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_entry_argument, application)
#define ENTRY_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_entry_answer, storage)
#define RUNTIME_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_runtime_argument, environment_count)
#define RUNTIME_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_runtime_answer, storage)
#define APPLICATION_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_application_argument, operation)
#define APPLICATION_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_application_answer, storage)
#define REGISTER_LOADER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_register_loader_argument, version)
#define REGISTER_LOADER_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_register_loader_answer, storage)
#define BY_ID_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_by_id_argument, api_base)
#define BY_ID_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_by_id_answer, storage)
#define ECOSYSTEM_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_ecosystem_argument, platform)
#define ECOSYSTEM_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_ecosystem_answer, storage)

static const char *const WARNING_KINDS[] = {NULL, "named-dependency", "unbound-service"};
static const char *const SOURCES[] = {NULL, "path", "acquired"};
static const char *const OPERATIONS[] = {NULL, "update", "remove"};

static int is_text(const char *text) {
    return text != NULL && text[0] != '\0';
}

static int is_place(const char *place) {
    return place != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place);
}

static spisor_status refuse_malformed(spisor_refusal *refusal, const char *what) {
    char detail[200];
    char fix[200];
    snprintf(detail, sizeof detail, "the argument carries no usable %s", what);
    snprintf(fix, sizeof fix, "send %s as install.h describes it", what);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

/** The index of `name` in `names`, a NULL-led table of `count`, or 0. */
static uint32_t index_of(const char *const *names, uint32_t count, const char *name) {
    for (uint32_t index = 1; name != NULL && index < count; index++) {
        if (strcmp(name, names[index]) == 0) {
            return index;
        }
    }
    return 0;
}

/** Whether `count` texts are all present, an array of none allowed to be NULL. */
static int are_texts(const char *const *texts, size_t count) {
    if (count > 0 && texts == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (texts[index] == NULL) {
            return 0;
        }
    }
    return 1;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static int add_texts(cJSON *object, const char *name, const char *const *texts, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, name);
    for (size_t index = 0; array != NULL && index < count; index++) {
        cJSON *text = cJSON_CreateString(texts[index]);
        if (text == NULL || !cJSON_AddItemToArray(array, text)) {
            cJSON_Delete(text);
            return 0;
        }
    }
    return array != NULL;
}

/** A call carrying `places` of the named absolute places, a NULL place left out; NULL when memory runs out. */
static cJSON *places_call(const char *const *names, const char *const *places, size_t count) {
    cJSON *call = cJSON_CreateObject();
    cJSON *object = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    for (size_t index = 0; object != NULL && index < count; index++) {
        if (!add_text(object, names[index], places[index])) {
            object = NULL;
        }
    }
    if (object == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Adds the item and its manifest packages, when there are any, to `call`, which it deletes on any failure. */
static spisor_status add_item(cJSON **call, const char *item, const spisor_value *manifest_packages) {
    spisor_status status = *call == NULL || !add_text(*call, "item", item) ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    if (status == SPISOR_OK && manifest_packages != NULL) {
        status = runtime_add_value_json(*call, "manifestPackages", manifest_packages, SPISOR_VALUE_RECORD);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

/** The item's call for packages, plan and detect, its places named as `names`; MALFORMED for packages no record. */
static spisor_status item_call(const char *const *names, const char *const *places, size_t count, const char *item,
                               const spisor_value *manifest_packages, cJSON **call) {
    *call = places_call(names, places, count);
    return add_item(call, item, manifest_packages);
}

/** The status a builder answered, refusing a MALFORMED one as the manifest packages. */
static spisor_status refuse_built(spisor_status status, spisor_refusal *refusal) {
    return status == SPISOR_MALFORMED ? refuse_malformed(refusal, "manifest packages") : status;
}

/**
 * OK for a well-formed value of kind, or an absent one when it is optional; MALFORMED, refused as `what`, for any
 * other; OUT_OF_MEMORY when checking it cannot. Run before a call is built, so a malformed argument is refused as such
 * whatever memory the build would have needed.
 */
static spisor_status check_value(const spisor_value *value, spisor_value_kind kind, int optional, const char *what,
                                 spisor_refusal *refusal) {
    spisor_status status;
    if (value == NULL) {
        return optional ? SPISOR_OK : refuse_malformed(refusal, what);
    }
    if (value->kind != (uint32_t)kind) {
        return refuse_malformed(refusal, what);
    }
    status = spisor_value_check(value);
    return status == SPISOR_MALFORMED ? refuse_malformed(refusal, what) : status;
}

static spisor_status check_manifest_packages(const spisor_value *manifest_packages, spisor_refusal *refusal) {
    return check_value(manifest_packages, SPISOR_VALUE_RECORD, 1, "manifest packages", refusal);
}

static spisor_status start_storage(void **storage) {
    return runtime_storage_take(storage, 1) == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status copy_text(void **storage, const char *text, const char **copy) {
    return runtime_storage_copy(storage, text, copy) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

/** The package `mapping`, {name, options?}, through `method` in the storage. */
static spisor_status fill_package(void **storage, const char *method, const cJSON *mapping,
                                  runtime_install_package *package) {
    spisor_status status = copy_text(storage, method, &package->method);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(mapping, "name"), &package->name);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(mapping, "options"),
                                       &package->options);
    }
    return status;
}

static spisor_status fill_warnings(const cJSON *answered, void *filled) {
    runtime_install_warnings_answer *answer = filled;
    const cJSON *warnings = cJSON_GetObjectItemCaseSensitive(answered, "warnings");
    runtime_install_warning *array;
    const cJSON *entry;
    spisor_status status = start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(warnings),
                                                           sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->warnings = array;
    cJSON_ArrayForEach(entry, warnings) {
        runtime_install_warning *warning = &array[answer->warning_count++];
        warning->kind = index_of(WARNING_KINDS, 3, text_of(entry, "kind"));
        status = copy_text(&answer->storage, text_of(entry, "subject"), &warning->subject);
        if (status == SPISOR_OK) {
            status = copy_text(&answer->storage, text_of(entry, "detail"), &warning->detail);
        }
        if (status != SPISOR_OK) {
            return status;
        }
    }
    return SPISOR_OK;
}

static spisor_status warnings(void *context, const runtime_install_warnings_argument *argument,
                              runtime_install_warnings_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, WARNINGS_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, WARNINGS_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's warnings");
    }
    status = check_value(argument->manifest, SPISOR_VALUE_RECORD, 0, "manifest", refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    call = cJSON_CreateObject();
    status = call == NULL ? SPISOR_OUT_OF_MEMORY
                          : runtime_add_value_json(call, "manifest", argument->manifest, SPISOR_VALUE_RECORD);
    if (status != SPISOR_OK) {
        cJSON_Delete(call);
        return status == SPISOR_MALFORMED ? refuse_malformed(refusal, "manifest") : status;
    }
    return runtime_serve_operation(runtime_install_warnings_operation, call, fill_warnings, answer,
                                   WARNINGS_ANSWER_SHAPE, refusal);
}

static spisor_status fill_packages(const cJSON *answered, void *filled) {
    runtime_install_packages_answer *answer = filled;
    const cJSON *packages = cJSON_GetObjectItemCaseSensitive(answered, "packages");
    runtime_install_package *array;
    const cJSON *mapping;
    spisor_status status = start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(packages),
                                                           sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->packages = array;
    cJSON_ArrayForEach(mapping, packages) {
        status = fill_package(&answer->storage, mapping->string, mapping, &array[answer->package_count++]);
        if (status != SPISOR_OK) {
            return status;
        }
    }
    return SPISOR_OK;
}

/** MALFORMED, refused, for an item argument without its item or with a portable place that is not absolute. */
static spisor_status check_item(const char *portable, const char *item, spisor_refusal *refusal) {
    if (!runtime_is_usable_home(portable)) {
        return runtime_refuse_place(refusal, "portable");
    }
    return is_text(item) ? SPISOR_OK : refuse_malformed(refusal, "item");
}

static spisor_status packages(void *context, const runtime_install_packages_argument *argument,
                              runtime_install_packages_answer *answer, spisor_refusal *refusal) {
    static const char *const names[1] = {"portable"};
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PACKAGES_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, PACKAGES_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's packages");
    }
    status = check_item(argument->portable, argument->item, refusal);
    if (status == SPISOR_OK) {
        status = check_manifest_packages(argument->manifest_packages, refusal);
    }
    if (status == SPISOR_OK) {
        status = refuse_built(
            item_call(names, &argument->portable, 1, argument->item, argument->manifest_packages, &call), refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_packages_operation, call, fill_packages,
                                                         answer, PACKAGES_ANSWER_SHAPE, refusal);
}

/** The step a plan answered, {"install-provider": entry} or {"install": {method, package?}}, in the storage. */
static spisor_status fill_step(void **storage, const cJSON *json, runtime_install_step *step) {
    const cJSON *provider = cJSON_GetObjectItemCaseSensitive(json, "install-provider");
    const cJSON *install = cJSON_GetObjectItemCaseSensitive(json, "install");
    if (provider != NULL) {
        step->kind = RUNTIME_INSTALL_PROVIDER_STEP;
        return runtime_storage_value(storage, provider, &step->provider);
    }
    step->kind = RUNTIME_INSTALL_PACKAGE_STEP;
    return fill_package(storage, text_of(install, "method"), cJSON_GetObjectItemCaseSensitive(install, "package"),
                        &step->package);
}

static spisor_status fill_plan(const cJSON *answered, void *filled) {
    runtime_install_plan_answer *answer = filled;
    const cJSON *steps = cJSON_GetObjectItemCaseSensitive(answered, "steps");
    runtime_install_step *array;
    const cJSON *step;
    spisor_status status = start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(steps),
                                                           sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->steps = array;
    cJSON_ArrayForEach(step, steps) {
        status = fill_step(&answer->storage, step, &array[answer->step_count++]);
        if (status != SPISOR_OK) {
            return status;
        }
    }
    return SPISOR_OK;
}

/** MALFORMED, refused, for a plan argument whose methods, providers or catalog are not as install.h describes. */
static spisor_status check_plan(const runtime_install_plan_argument *argument, spisor_refusal *refusal) {
    spisor_status status = check_item(argument->portable, argument->item, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    if (!is_text(argument->chosen)) {
        return refuse_malformed(refusal, "chosen method");
    }
    if (!are_texts(argument->methods, argument->method_count)) {
        return refuse_malformed(refusal, "methods");
    }
    if (!are_texts(argument->installed_providers, argument->installed_provider_count)) {
        return refuse_malformed(refusal, "installed providers");
    }
    status = check_manifest_packages(argument->manifest_packages, refusal);
    return status == SPISOR_OK ? check_value(argument->catalog, SPISOR_VALUE_LIST, 1, "catalog", refusal) : status;
}

/** Adds the plan's methods, chosen method, installed providers and catalog to `call`, which it deletes on failure. */
static spisor_status add_plan(cJSON **call, const runtime_install_plan_argument *argument, spisor_refusal *refusal) {
    spisor_status status = add_texts(*call, "methods", argument->methods, argument->method_count) &&
                                   add_text(*call, "chosen", argument->chosen) &&
                                   add_texts(*call, "installedProviders", argument->installed_providers,
                                             argument->installed_provider_count)
                               ? SPISOR_OK
                               : SPISOR_OUT_OF_MEMORY;
    if (status == SPISOR_OK && argument->catalog != NULL) {
        status = runtime_add_value_json(*call, "catalog", argument->catalog, SPISOR_VALUE_LIST);
        if (status == SPISOR_MALFORMED) {
            status = refuse_malformed(refusal, "catalog");
        }
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status plan(void *context, const runtime_install_plan_argument *argument,
                          runtime_install_plan_answer *answer, spisor_refusal *refusal) {
    static const char *const names[1] = {"portable"};
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PLAN_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, PLAN_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's plan");
    }
    status = check_plan(argument, refusal);
    if (status == SPISOR_OK) {
        status = refuse_built(
            item_call(names, &argument->portable, 1, argument->item, argument->manifest_packages, &call), refusal);
    }
    if (status == SPISOR_OK) {
        status = add_plan(&call, argument, refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_plan_operation, call, fill_plan, answer,
                                                         PLAN_ANSWER_SHAPE, refusal);
}

/** A detection, {method, item, package?, options?, answer?: {installed, version?}, refused?}, in the storage. */
static spisor_status fill_detection(void **storage, const cJSON *json, runtime_install_detection *detection) {
    const cJSON *found = cJSON_GetObjectItemCaseSensitive(json, "answer");
    spisor_status status = copy_text(storage, text_of(json, "item"), &detection->item);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "method"), &detection->package.method);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "package"), &detection->package.name);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "options"),
                                       &detection->package.options);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(found, "version"), &detection->version);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "refused"), &detection->refused);
    }
    detection->installed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found, "installed")) ? 1u : 0u;
    return status;
}

static spisor_status fill_detect(const cJSON *answered, void *filled) {
    runtime_install_detect_answer *answer = filled;
    const cJSON *detections = cJSON_GetObjectItemCaseSensitive(answered, "detections");
    runtime_install_detection *array;
    const cJSON *detection;
    spisor_status status = start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(detections),
                                                           sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->detections = array;
    cJSON_ArrayForEach(detection, detections) {
        status = fill_detection(&answer->storage, detection, &array[answer->detection_count++]);
        if (status != SPISOR_OK) {
            return status;
        }
    }
    return SPISOR_OK;
}

static spisor_status detect(void *context, const runtime_install_detect_argument *argument,
                            runtime_install_detect_answer *answer, spisor_refusal *refusal) {
    static const char *const names[2] = {"portable", "cache"};
    const char *places[2];
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, DETECT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, DETECT_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's detect");
    }
    status = check_item(argument->portable, argument->item, refusal);
    if (status == SPISOR_OK && !is_place(argument->cache)) {
        status = runtime_refuse_place(refusal, "cache");
    }
    if (status == SPISOR_OK) {
        status = check_manifest_packages(argument->manifest_packages, refusal);
    }
    places[0] = argument->portable;
    places[1] = argument->cache;
    if (status == SPISOR_OK) {
        status = refuse_built(item_call(names, places, 2, argument->item, argument->manifest_packages, &call), refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_detect_operation, call, fill_detect, answer,
                                                         DETECT_ANSWER_SHAPE, refusal);
}

/** Adds `environment` to `call` as the JSON object runtime's path rules read, MALFORMED refused as such. */
static spisor_status add_environment(cJSON *call, const runtime_variable *environment, size_t count,
                                     spisor_refusal *refusal) {
    cJSON *json = NULL;
    spisor_status status = runtime_environment_json(environment, count, &json);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    if (status == SPISOR_OK && !cJSON_AddItemToObject(call, "environment", json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

/** MALFORMED, refused, for a place of `names` that is not absolute; OK when every one is. */
static spisor_status check_places(const char *const *names, const char *const *places, size_t count,
                                  spisor_refusal *refusal) {
    for (size_t index = 0; index < count; index++) {
        if (!is_place(places[index])) {
            return runtime_refuse_place(refusal, names[index]);
        }
    }
    return SPISOR_OK;
}

/** check_value for an application, which must also carry a non-empty text id. */
static spisor_status check_application(const spisor_value *application, spisor_refusal *refusal) {
    const spisor_value *id;
    spisor_status status = check_value(application, SPISOR_VALUE_RECORD, 0, "application", refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    id = spisor_value_member(application, "id", 2);
    return id != NULL && id->kind == SPISOR_VALUE_TEXT && id->length > 0 ? SPISOR_OK
                                                                         : refuse_malformed(refusal, "application");
}

static spisor_status fill_entry(const cJSON *answered, void *filled) {
    runtime_install_entry_answer *answer = filled;
    const cJSON *sequences = cJSON_GetObjectItemCaseSensitive(answered, "sequences");
    uint64_t *array;
    const cJSON *sequence;
    spisor_status status = start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(sequences),
                                                           sizeof *array, (void **)&array)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->written = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "written")) ? 1u : 0u;
    answer->runs_on_javascript_host =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "runsOnJavascriptHost")) ? 1u : 0u;
    answer->sequences = array;
    cJSON_ArrayForEach(sequence, sequences) {
        array[answer->sequence_count++] = (uint64_t)sequence->valuedouble;
    }
    status = copy_text(&answer->storage, text_of(answered, "message"), &answer->detail);
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(answered, "fix"), &answer->fix) : status;
}

/** The entry's call, {places: {state, cache}, loader: {id, application, home}}. */
static spisor_status entry_call(const runtime_install_entry_argument *argument, cJSON **call) {
    static const char *const names[2] = {"state", "cache"};
    const char *places[2];
    cJSON *loader;
    spisor_status status;
    places[0] = argument->state;
    places[1] = argument->cache;
    *call = places_call(names, places, 2);
    loader = *call == NULL ? NULL : cJSON_AddObjectToObject(*call, "loader");
    status = loader != NULL && add_text(loader, "id", argument->loader) && add_text(loader, "home", argument->home)
                 ? runtime_add_value_json(loader, "application", argument->application, SPISOR_VALUE_RECORD)
                 : SPISOR_OUT_OF_MEMORY;
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status entry(void *context, const runtime_install_entry_argument *argument,
                           runtime_install_entry_answer *answer, spisor_refusal *refusal) {
    static const char *const names[3] = {"state", "cache", "loader home"};
    const char *places[3];
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ENTRY_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ENTRY_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's entry");
    }
    places[0] = argument->state;
    places[1] = argument->cache;
    places[2] = argument->home;
    status = check_places(names, places, 3, refusal);
    if (status == SPISOR_OK && !is_text(argument->loader)) {
        status = refuse_malformed(refusal, "loader");
    }
    if (status == SPISOR_OK) {
        status = check_application(argument->application, refusal);
    }
    if (status == SPISOR_OK) {
        status = entry_call(argument, &call);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_entry_operation, call, fill_entry, answer,
                                                         ENTRY_ANSWER_SHAPE, refusal);
}

/** A JavaScript runtime as install's runtime answers it, {name, version, executable, fetched, source}. */
static spisor_status fill_javascript(void **storage, const cJSON *json, runtime_install_javascript *javascript) {
    spisor_status status = copy_text(storage, text_of(json, "name"), &javascript->name);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "version"), &javascript->version);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "executable"), &javascript->executable);
    }
    javascript->fetched = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "fetched")) ? 1u : 0u;
    javascript->source = index_of(SOURCES, 3, text_of(json, "source"));
    return status;
}

static spisor_status fill_runtime(const cJSON *answered, void *filled) {
    runtime_install_runtime_answer *answer = filled;
    spisor_status status = start_storage(&answer->storage);
    return status == SPISOR_OK ? fill_javascript(&answer->storage, answered, &answer->javascript) : status;
}

static spisor_status runtime_call(const runtime_install_runtime_argument *argument, cJSON **call,
                                  spisor_refusal *refusal) {
    spisor_status status;
    *call = cJSON_CreateObject();
    status = *call == NULL || !add_text(*call, "cache", argument->cache) ||
                     !add_text(*call, "platform", argument->platform)
                 ? SPISOR_OUT_OF_MEMORY
                 : add_environment(*call, argument->environment, argument->environment_count, refusal);
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status runtime(void *context, const runtime_install_runtime_argument *argument,
                             runtime_install_runtime_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RUNTIME_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RUNTIME_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's runtime");
    }
    status = is_place(argument->cache) ? SPISOR_OK : runtime_refuse_place(refusal, "cache");
    if (status == SPISOR_OK && argument->platform != NULL && !runtime_is_platform_key(argument->platform)) {
        status = refuse_malformed(refusal, "platform");
    }
    if (status == SPISOR_OK) {
        status = runtime_call(argument, &call, refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_runtime_operation, call, fill_runtime, answer,
                                                         RUNTIME_ANSWER_SHAPE, refusal);
}

static spisor_status fill_application(const cJSON *answered, void *filled) {
    runtime_install_application_answer *answer = filled;
    const cJSON *unrecorded = cJSON_GetObjectItemCaseSensitive(answered, "unrecorded");
    spisor_status status = start_storage(&answer->storage);
    answer->operation = index_of(OPERATIONS, 3, text_of(answered, "operation"));
    if (status == SPISOR_OK) {
        status = copy_text(&answer->storage, text_of(answered, "version"), &answer->version);
    }
    if (status == SPISOR_OK) {
        status = copy_text(&answer->storage, text_of(unrecorded, "message"), &answer->unrecorded_detail);
    }
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(unrecorded, "fix"), &answer->unrecorded_fix)
                               : status;
}

static spisor_status application(void *context, const runtime_install_application_argument *argument,
                                 runtime_install_application_answer *answer, spisor_refusal *refusal) {
    static const char *const names[3] = {"portable", "state", "cache"};
    const char *places[3];
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, APPLICATION_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, APPLICATION_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's application");
    }
    places[0] = argument->portable;
    places[1] = argument->state;
    places[2] = argument->cache;
    status = check_places(names, places, 3, refusal);
    if (status == SPISOR_OK && !is_text(argument->loader)) {
        status = refuse_malformed(refusal, "loader");
    }
    if (status == SPISOR_OK && argument->operation != RUNTIME_INSTALL_UPDATE &&
        argument->operation != RUNTIME_INSTALL_REMOVE) {
        status = refuse_malformed(refusal, "operation");
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = places_call(names, places, 3);
    if (call != NULL && (!add_text(call, "loader", argument->loader) ||
                         !add_text(call, "operation", OPERATIONS[argument->operation]))) {
        cJSON_Delete(call);
        call = NULL;
    }
    return runtime_serve_operation(runtime_install_application_operation, call, fill_application, answer,
                                   APPLICATION_ANSWER_SHAPE, refusal);
}

static spisor_status fill_refused_commands(void **storage, const cJSON *array,
                                           const runtime_install_refused_command **filled, size_t *count) {
    runtime_install_refused_command *commands;
    const cJSON *refused;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), sizeof *commands,
                                    (void **)&commands)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = commands;
    cJSON_ArrayForEach(refused, array) {
        runtime_install_refused_command *command = &commands[(*count)++];
        if (!runtime_storage_copy(storage, text_of(refused, "name"), &command->name) ||
            !runtime_storage_copy(storage, text_of(refused, "detail"), &command->detail) ||
            !runtime_storage_copy(storage, text_of(refused, "fix"), &command->fix)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_register_loader(const cJSON *answered, void *filled) {
    runtime_install_register_loader_answer *answer = filled;
    spisor_status status = start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = runtime_storage_provenance(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "provenance"),
                                            &answer->provenance);
    }
    if (status == SPISOR_OK &&
        !runtime_storage_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "commands"),
                               &answer->commands, &answer->command_count)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status == SPISOR_OK ? fill_refused_commands(&answer->storage,
                                                       cJSON_GetObjectItemCaseSensitive(answered, "refusedCommands"),
                                                       &answer->refused_commands, &answer->refused_command_count)
                               : status;
}

static spisor_status register_loader_call(const runtime_install_register_loader_argument *argument,
                                          const char *const *names, const char *const *places, cJSON **call,
                                          spisor_refusal *refusal) {
    spisor_status status;
    *call = places_call(names, places, 3);
    status = *call == NULL || !add_text(*call, "id", argument->id) || !add_text(*call, "version", argument->version)
                 ? SPISOR_OUT_OF_MEMORY
                 : add_environment(*call, argument->environment, argument->environment_count, refusal);
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status register_loader(void *context, const runtime_install_register_loader_argument *argument,
                                     runtime_install_register_loader_answer *answer, spisor_refusal *refusal) {
    static const char *const names[3] = {"portable", "state", "cache"};
    const char *places[3];
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, REGISTER_LOADER_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, REGISTER_LOADER_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's register_loader");
    }
    places[0] = argument->portable;
    places[1] = argument->state;
    places[2] = argument->cache;
    status = check_places(names, places, 3, refusal);
    if (status == SPISOR_OK && (!is_text(argument->id) || !is_text(argument->version))) {
        status = refuse_malformed(refusal, is_text(argument->id) ? "version" : "loader id");
    }
    if (status == SPISOR_OK) {
        status = register_loader_call(argument, names, places, &call, refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_register_loader_operation, call,
                                                         fill_register_loader, answer, REGISTER_LOADER_ANSWER_SHAPE,
                                                         refusal);
}

static spisor_status copy_member(void **storage, const cJSON *answered, const char *name, const char **copy) {
    return copy_text(storage, text_of(answered, name), copy);
}

static uint32_t is_true(const cJSON *answered, const char *name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, name)) ? 1u : 0u;
}

static spisor_status fill_by_id(const cJSON *answered, void *filled) {
    static const char *const names[7] = {"id", "name", "fullName", "primary", "recordedPrimary", "version", "commit"};
    runtime_install_by_id_answer *answer = filled;
    const char **copies[7];
    spisor_status status = start_storage(&answer->storage);
    copies[0] = &answer->id;
    copies[1] = &answer->name;
    copies[2] = &answer->full_name;
    copies[3] = &answer->primary;
    copies[4] = &answer->recorded_primary;
    copies[5] = &answer->version;
    copies[6] = &answer->commit;
    for (size_t index = 0; status == SPISOR_OK && index < 7; index++) {
        status = copy_member(&answer->storage, answered, names[index], copies[index]);
    }
    if (status == SPISOR_OK) {
        status = copy_member(&answer->storage, answered, "sha256", &answer->sha256);
    }
    if (status == SPISOR_OK &&
        !runtime_storage_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "ids"), &answer->ids,
                               &answer->id_count)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    answer->primary_confirmed = is_true(answered, "primaryConfirmed");
    answer->needs_user_acceptance = is_true(answered, "needsUserAcceptance");
    return status;
}

static int is_repo_id(const char *id) {
    runtime_identity parsed;
    return is_text(id) && runtime_identity_parse_id(id, &parsed);
}

/** Whether every optional text of the argument is NULL or not empty. */
static int are_optional_texts(const runtime_install_by_id_argument *argument) {
    const char *const optional[4] = {argument->version, argument->last_known, argument->accept_primary,
                                     argument->api_base};
    for (size_t index = 0; index < 4; index++) {
        if (optional[index] != NULL && optional[index][0] == '\0') {
            return 0;
        }
    }
    return 1;
}

static spisor_status by_id_call(const runtime_install_by_id_argument *argument, cJSON **call) {
    *call = cJSON_CreateObject();
    if (*call == NULL || !add_text(*call, "id", argument->id) || !add_text(*call, "cache", argument->cache) ||
        !add_text(*call, "pluginDir", argument->plugin_directory) || !add_text(*call, "version", argument->version) ||
        !add_text(*call, "lastKnown", argument->last_known) ||
        !add_text(*call, "acceptPrimary", argument->accept_primary) ||
        !add_text(*call, "apiBase", argument->api_base)) {
        cJSON_Delete(*call);
        *call = NULL;
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

/**
 * Serves byId, its answer filled before the record of the primary is written whenever there is one to write, so an
 * answer that runs out of memory leaves no record; one answered without a record, a user acceptance, fills after.
 */
static spisor_status serve_by_id(cJSON *call, runtime_install_by_id_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = call == NULL ? NULL : runtime_install_by_id_taking(call, fill_by_id, answer, &operation_refusal);
    spisor_status status = SPISOR_OK;
    cJSON_Delete(call);
    if (answered == NULL) {
        runtime_release_stored_answer(answer, BY_ID_ANSWER_SHAPE);
        status = runtime_refuse_json(refusal, operation_refusal);
    } else if (answer->storage == NULL) {
        status = fill_by_id(answered, answer);
        if (status != SPISOR_OK) {
            runtime_release_stored_answer(answer, BY_ID_ANSWER_SHAPE);
        }
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status by_id(void *context, const runtime_install_by_id_argument *argument,
                           runtime_install_by_id_answer *answer, spisor_refusal *refusal) {
    static const char *const names[2] = {"cache", "plugin directory"};
    const char *places[2];
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, BY_ID_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, BY_ID_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's by_id");
    }
    places[0] = argument->cache;
    places[1] = argument->plugin_directory;
    status = check_places(names, places, 2, refusal);
    if (status == SPISOR_OK && !is_repo_id(argument->id)) {
        status = refuse_malformed(refusal, "id");
    }
    if (status == SPISOR_OK && !are_optional_texts(argument)) {
        status = refuse_malformed(refusal, "version, last known name, accepted primary or api base");
    }
    if (status == SPISOR_OK) {
        status = by_id_call(argument, &call);
    }
    return status != SPISOR_OK ? status : serve_by_id(call, answer, refusal);
}

typedef spisor_status (*row_filler)(void **storage, const cJSON *json, void *row);

/** Each element of `array` as a zeroed row of `width` bytes in the storage, filled by `fill`. */
static spisor_status fill_rows(void **storage, const cJSON *array, size_t width, row_filler fill, void **rows,
                               size_t *count) {
    char *taken;
    const cJSON *json;
    spisor_status status = SPISOR_OK;
    *count = 0;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), width, (void **)&taken)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *rows = taken;
    cJSON_ArrayForEach(json, array) {
        if (status == SPISOR_OK) {
            char *row = taken + width * (*count)++;
            memset(row, 0, width);
            status = fill(storage, json, row);
        }
    }
    return status;
}

static spisor_status copy_texts(void **storage, const cJSON *array, const char *const **texts, size_t *count) {
    return runtime_storage_texts(storage, array, texts, count) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_unavailable_under(void **storage, const cJSON *json, runtime_install_unavailable *row,
                                            const char *id_name) {
    spisor_status status = copy_text(storage, text_of(json, id_name), &row->id);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "message"), &row->detail);
    }
    return status == SPISOR_OK ? copy_text(storage, text_of(json, "fix"), &row->fix) : status;
}

static spisor_status fill_unavailable(void **storage, const cJSON *json, void *row) {
    return fill_unavailable_under(storage, json, row, "id");
}

static spisor_status fill_method_unavailable(void **storage, const cJSON *json, void *row) {
    return fill_unavailable_under(storage, json, row, "method");
}

static spisor_status fill_fetched(void **storage, const cJSON *json, void *row) {
    runtime_install_fetched *fetched = row;
    spisor_status status = copy_text(storage, text_of(json, "id"), &fetched->id);
    return status == SPISOR_OK ? copy_text(storage, text_of(json, "version"), &fetched->version) : status;
}

static spisor_status fill_origin(void **storage, const cJSON *json, runtime_install_origin *origin) {
    spisor_status status = copy_text(storage, text_of(json, "method"), &origin->package.method);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "package"), &origin->package.name);
    }
    if (status == SPISOR_OK) {
        status = runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "options"),
                                       &origin->package.options);
    }
    return status == SPISOR_OK ? copy_text(storage, text_of(json, "version"), &origin->version) : status;
}

static spisor_status fill_settled_application(void **storage, const cJSON *json, void *row) {
    static const char *const states[] = {NULL, "installed", "detected", "missing", "skipped", "failed", "unavailable"};
    runtime_install_application *application = row;
    void *unavailable = NULL;
    spisor_status status = copy_text(storage, text_of(json, "id"), &application->id);
    application->state = index_of(states, 7, text_of(json, "state"));
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "installedThrough"), &application->installed_through);
    }
    if (status == SPISOR_OK) {
        status = fill_origin(storage, cJSON_GetObjectItemCaseSensitive(json, "origin"), &application->origin);
    }
    if (status == SPISOR_OK) {
        status = copy_texts(storage, cJSON_GetObjectItemCaseSensitive(json, "offered"), &application->offered,
                            &application->offered_count);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "flag"), &application->flag);
    }
    if (status == SPISOR_OK) {
        status = fill_rows(storage, cJSON_GetObjectItemCaseSensitive(json, "unavailable"),
                           sizeof(runtime_install_unavailable), fill_method_unavailable, &unavailable,
                           &application->unavailable_count);
        application->unavailable = unavailable;
    }
    return status;
}

static spisor_status fill_loader_entry(void **storage, const cJSON *entry, runtime_install_loader *loader) {
    spisor_status status = copy_text(storage, text_of(entry, "message"), &loader->entry_detail);
    loader->entry_written = is_true(entry, "written");
    loader->entry_runs_on_javascript_host = is_true(entry, "runsOnJavascriptHost");
    return status == SPISOR_OK ? copy_text(storage, text_of(entry, "fix"), &loader->entry_fix) : status;
}

static spisor_status fill_loader(void **storage, const cJSON *json, void *row) {
    runtime_install_loader *loader = row;
    spisor_status status = copy_text(storage, text_of(json, "id"), &loader->id);
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "application"), &loader->application);
    }
    if (status == SPISOR_OK) {
        status = copy_text(storage, text_of(json, "home"), &loader->home);
    }
    if (status == SPISOR_OK) {
        status = copy_texts(storage, cJSON_GetObjectItemCaseSensitive(json, "commands"), &loader->commands,
                            &loader->command_count);
    }
    if (status == SPISOR_OK) {
        status = fill_refused_commands(storage, cJSON_GetObjectItemCaseSensitive(json, "refusedCommands"),
                                       &loader->refused_commands, &loader->refused_command_count);
    }
    return status == SPISOR_OK ? fill_loader_entry(storage, cJSON_GetObjectItemCaseSensitive(json, "entry"), loader)
                               : status;
}

static spisor_status fill_missing(void **storage, const cJSON *json, void *row) {
    runtime_install_missing *missing = row;
    spisor_status status = copy_text(storage, text_of(json, "application"), &missing->application);
    return status == SPISOR_OK ? copy_texts(storage, cJSON_GetObjectItemCaseSensitive(json, "methods"),
                                            &missing->methods, &missing->method_count)
                               : status;
}

/** The report member `name`'s rows, each of `width` bytes filled by `fill`, into `*rows` and `*count`. */
static spisor_status fill_report_rows(runtime_install_ecosystem_answer *answer, const cJSON *report, const char *name,
                                      size_t width, row_filler fill, const void **rows, size_t *count) {
    void *filled = NULL;
    spisor_status status = fill_rows(&answer->storage, cJSON_GetObjectItemCaseSensitive(report, name), width, fill,
                                     &filled, count);
    *rows = filled;
    return status;
}

static spisor_status fill_ecosystem_rows(runtime_install_ecosystem_answer *answer, const cJSON *report) {
    const void *rows[5] = {NULL, NULL, NULL, NULL, NULL};
    spisor_status status = fill_report_rows(answer, report, "installed", sizeof(runtime_install_fetched),
                                            fill_fetched, &rows[0], &answer->installed_count);
    if (status == SPISOR_OK) {
        status = fill_report_rows(answer, report, "unavailable", sizeof(runtime_install_unavailable),
                                  fill_unavailable, &rows[1], &answer->unavailable_count);
    }
    if (status == SPISOR_OK) {
        status = fill_report_rows(answer, report, "applications", sizeof(runtime_install_application),
                                  fill_settled_application, &rows[2], &answer->application_count);
    }
    if (status == SPISOR_OK) {
        status = fill_report_rows(answer, report, "loaders", sizeof(runtime_install_loader), fill_loader, &rows[3],
                                  &answer->loader_count);
    }
    if (status == SPISOR_OK) {
        status = fill_report_rows(answer, report, "missing", sizeof(runtime_install_missing), fill_missing,
                                  &rows[4], &answer->missing_count);
    }
    answer->installed = rows[0];
    answer->unavailable = rows[1];
    answer->applications = rows[2];
    answer->loaders = rows[3];
    answer->missing = rows[4];
    return status;
}

static spisor_status fill_acquired(runtime_install_ecosystem_answer *answer, const cJSON *javascript) {
    runtime_install_javascript *acquired;
    if (javascript == NULL) {
        return SPISOR_OK;
    }
    acquired = runtime_storage_take(&answer->storage, sizeof *acquired);
    if (acquired == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    memset(acquired, 0, sizeof *acquired);
    answer->javascript = acquired;
    return fill_javascript(&answer->storage, javascript, acquired);
}

/** Fills the answer from the report, replacing what an earlier take of the same install filled. */
static spisor_status fill_ecosystem(const cJSON *report, void *filled) {
    runtime_install_ecosystem_answer *answer = filled;
    spisor_status status;
    runtime_release_stored_answer(answer, ECOSYSTEM_ANSWER_SHAPE);
    status = start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = fill_ecosystem_rows(answer, report);
    }
    if (status == SPISOR_OK) {
        status = copy_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(report, "cached"), &answer->cached,
                            &answer->cached_count);
    }
    if (status == SPISOR_OK) {
        status = copy_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(report, "enabled"), &answer->enabled,
                            &answer->enabled_count);
    }
    return status == SPISOR_OK ? fill_acquired(answer, cJSON_GetObjectItemCaseSensitive(report, "javascript"))
                               : status;
}

static int are_choices(const runtime_install_choice *choices, size_t count) {
    if (count > 0 && choices == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_text(choices[index].application) || !is_text(choices[index].method)) {
            return 0;
        }
    }
    return 1;
}

static spisor_status check_ecosystem(const runtime_install_ecosystem_argument *argument, const char *const *names,
                                     const char *const *places, spisor_refusal *refusal) {
    spisor_status status = check_places(names, places, 3, refusal);
    if (status == SPISOR_OK && !runtime_catalog_are_sources(argument->sources, argument->source_count)) {
        status = refuse_malformed(refusal, "sources");
    }
    if (status == SPISOR_OK && !are_choices(argument->choices, argument->choice_count)) {
        status = refuse_malformed(refusal, "choices");
    }
    if (status == SPISOR_OK && (!are_texts(argument->defaults, argument->default_count) ||
                                !are_texts(argument->requested, argument->requested_count))) {
        status = refuse_malformed(refusal, "default or requested plugins");
    }
    if (status == SPISOR_OK && argument->platform != NULL && !runtime_is_platform_key(argument->platform)) {
        status = refuse_malformed(refusal, "platform");
    }
    return status;
}

static int add_choices(cJSON *call, const runtime_install_choice *choices, size_t count) {
    cJSON *object = cJSON_AddObjectToObject(call, "choices");
    for (size_t index = 0; object != NULL && index < count; index++) {
        if (!add_text(object, choices[index].application, choices[index].method)) {
            return 0;
        }
    }
    return object != NULL;
}

static int add_ecosystem_members(cJSON *call, const runtime_install_ecosystem_argument *argument) {
    return runtime_catalog_add_sources(call, argument->sources, argument->source_count) == SPISOR_OK &&
           add_choices(call, argument->choices, argument->choice_count) &&
           add_texts(call, "defaults", argument->defaults, argument->default_count) &&
           add_texts(call, "requested", argument->requested, argument->requested_count) &&
           add_text(call, "platform", argument->platform);
}

/** The ecosystem's call, its environment read first so a malformed one is refused before the call is built. */
static spisor_status ecosystem_call(const runtime_install_ecosystem_argument *argument, const char *const *names,
                                    const char *const *places, cJSON **call, spisor_refusal *refusal) {
    cJSON *environment = NULL;
    spisor_status status = runtime_environment_json(argument->environment, argument->environment_count, &environment);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_environment(refusal);
    }
    *call = status == SPISOR_OK ? places_call(names, places, 3) : NULL;
    if (*call == NULL || !cJSON_AddItemToObject(*call, "environment", environment)) {
        cJSON_Delete(environment);
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (status == SPISOR_OK && !add_ecosystem_members(*call, argument)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

/** Serves ecosystem, its answer filled before the running versions are recorded, the install's last write. */
static spisor_status serve_ecosystem(cJSON *call, runtime_install_ecosystem_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_install_ecosystem_taking(call, fill_ecosystem, answer, &operation_refusal);
    spisor_status status = SPISOR_OK;
    cJSON_Delete(call);
    if (answered == NULL) {
        runtime_release_stored_answer(answer, ECOSYSTEM_ANSWER_SHAPE);
        status = runtime_refuse_json(refusal, operation_refusal);
    } else if (answer->storage == NULL) {
        status = fill_ecosystem(answered, answer);
        if (status != SPISOR_OK) {
            runtime_release_stored_answer(answer, ECOSYSTEM_ANSWER_SHAPE);
        }
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status ecosystem(void *context, const runtime_install_ecosystem_argument *argument,
                               runtime_install_ecosystem_answer *answer, spisor_refusal *refusal) {
    static const char *const names[3] = {"portable", "state", "cache"};
    const char *places[3];
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ECOSYSTEM_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ECOSYSTEM_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "install's ecosystem");
    }
    places[0] = argument->portable;
    places[1] = argument->state;
    places[2] = argument->cache;
    status = check_ecosystem(argument, names, places, refusal);
    if (status == SPISOR_OK) {
        status = ecosystem_call(argument, names, places, &call, refusal);
    }
    return status != SPISOR_OK ? status : serve_ecosystem(call, answer, refusal);
}

static void release_warnings_answer(void *context, runtime_install_warnings_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, WARNINGS_ANSWER_SHAPE);
}

static void release_packages_answer(void *context, runtime_install_packages_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PACKAGES_ANSWER_SHAPE);
}

static void release_plan_answer(void *context, runtime_install_plan_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PLAN_ANSWER_SHAPE);
}

static void release_detect_answer(void *context, runtime_install_detect_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, DETECT_ANSWER_SHAPE);
}

static void release_entry_answer(void *context, runtime_install_entry_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ENTRY_ANSWER_SHAPE);
}

static void release_runtime_answer(void *context, runtime_install_runtime_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RUNTIME_ANSWER_SHAPE);
}

static void release_application_answer(void *context, runtime_install_application_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, APPLICATION_ANSWER_SHAPE);
}

static void release_register_loader_answer(void *context, runtime_install_register_loader_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REGISTER_LOADER_ANSWER_SHAPE);
}

static void release_by_id_answer(void *context, runtime_install_by_id_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, BY_ID_ANSWER_SHAPE);
}

static void release_ecosystem_answer(void *context, runtime_install_ecosystem_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ECOSYSTEM_ANSWER_SHAPE);
}

static const runtime_install_definition definition = {
    sizeof(runtime_install_definition),
    NULL,
    warnings,
    packages,
    plan,
    detect,
    entry,
    runtime,
    application,
    register_loader,
    by_id,
    ecosystem,
    release_warnings_answer,
    release_packages_answer,
    release_plan_answer,
    release_detect_answer,
    release_entry_answer,
    release_runtime_answer,
    release_application_answer,
    release_register_loader_answer,
    release_by_id_answer,
    release_ecosystem_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_install_definition_of(void) {
    return &definition;
}
