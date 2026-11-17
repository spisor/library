#include "runtime/install.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "install.h"
#include "install_methods.h"
#include "install_warnings.h"
#include "paths.h"
#include "spisor/path.h"

#define WARNINGS_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_warnings_argument, manifest)
#define WARNINGS_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_warnings_answer, storage)
#define PACKAGES_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_packages_argument, manifest_packages)
#define PACKAGES_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_packages_answer, storage)
#define PLAN_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_plan_argument, catalog)
#define PLAN_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_plan_answer, storage)
#define DETECT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_detect_argument, manifest_packages)
#define DETECT_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_install_detect_answer, storage)

static const char *const WARNING_KINDS[] = {NULL, "named-dependency", "unbound-service"};

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
    return SPISOR_OK;
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
    places[0] = argument->portable;
    places[1] = argument->cache;
    if (status == SPISOR_OK) {
        status = refuse_built(item_call(names, places, 2, argument->item, argument->manifest_packages, &call), refusal);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_install_detect_operation, call, fill_detect, answer,
                                                         DETECT_ANSWER_SHAPE, refusal);
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

static const runtime_install_definition definition = {
    sizeof(runtime_install_definition),
    NULL,
    warnings,
    packages,
    plan,
    detect,
    release_warnings_answer,
    release_packages_answer,
    release_plan_answer,
    release_detect_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_install_definition_of(void) {
    return &definition;
}
