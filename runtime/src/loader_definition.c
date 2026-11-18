#include "runtime/loader.h"

#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "host_references.h"
#include "json_service.h"
#include "json_value.h"
#include "loader.h"

#define HEADER "the loader header"
#define CHECK_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_check_argument, standard)
#define CHECK_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_check_answer, storage)
#define HOME_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_home_argument, environment_count)
#define HOME_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_home_answer, storage)
#define INSTALLED_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_installed_argument, loader)
#define INSTALLED_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_loader_installed_answer, storage)

static const char *const HOME_SOURCES[] = {NULL, "override", "native", "xdg", "candidate", "default"};

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
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

/** {standard}, `*call` NULL unless the answer is OK; MALFORMED for a value that is not well-formed. */
static spisor_status check_call(const runtime_loader_check_argument *argument, cJSON **call) {
    cJSON *standard = NULL;
    spisor_status status = spisor_json_of_value(argument->standard, &standard);
    *call = status == SPISOR_OK ? cJSON_CreateObject() : NULL;
    if (status == SPISOR_OK && (*call == NULL || !cJSON_AddItemToObject(*call, "standard", standard))) {
        cJSON_Delete(*call);
        *call = NULL;
        status = SPISOR_OUT_OF_MEMORY;
    }
    if (*call == NULL) {
        cJSON_Delete(standard);
    }
    return status;
}

static spisor_status fill_check(const cJSON *answered, void *filled) {
    runtime_loader_check_answer *answer = filled;
    const cJSON *issues = cJSON_GetObjectItemCaseSensitive(answered, "issues");
    const cJSON *entry;
    runtime_loader_issue *copies;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status != SPISOR_OK || !runtime_storage_take_array(&answer->storage, (size_t)cJSON_GetArraySize(issues),
                                                           sizeof *copies, (void **)&copies)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->issues = copies;
    cJSON_ArrayForEach(entry, issues) {
        runtime_loader_issue *each = &copies[answer->issue_count++];
        if (!runtime_storage_copy(&answer->storage, text_of(entry, "path"), &each->path) ||
            !runtime_storage_copy(&answer->storage, text_of(entry, "detail"), &each->detail) ||
            !runtime_storage_copy(&answer->storage, text_of(entry, "fix"), &each->fix)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status check(void *context, const runtime_loader_check_argument *argument,
                           runtime_loader_check_answer *answer, spisor_refusal *refusal) {
    cJSON *call = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CHECK_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, CHECK_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "loader's check");
    }
    if (argument->standard == NULL) {
        return runtime_refuse_member(refusal, "standard", HEADER);
    }
    status = check_call(argument, &call);
    if (status == SPISOR_MALFORMED) {
        return runtime_refuse_member(refusal, "standard", HEADER);
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_loader_check_operation, call, fill_check, answer,
                                                         CHECK_ANSWER_SHAPE, refusal);
}

static int is_optional_text(const char *text) {
    return text == NULL || runtime_is_text(text);
}

static spisor_status check_home(const runtime_loader_home_argument *argument, spisor_refusal *refusal) {
    if (!runtime_is_environment(argument->environment, argument->environment_count)) {
        return runtime_refuse_environment(refusal);
    }
    if (!is_optional_text(argument->env_override) || !is_optional_text(argument->native_env) ||
        !is_optional_text(argument->xdg_subdir)) {
        return runtime_refuse_member(refusal, "env_override, native_env or xdg_subdir", HEADER);
    }
    if (argument->candidate_count == 0 || !runtime_are_texts(argument->candidates, argument->candidate_count, 0)) {
        return runtime_refuse_member(refusal, "candidates", HEADER);
    }
    return SPISOR_OK;
}

static int add_candidates(cJSON *home, const char *const *candidates, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(home, "candidates");
    int built = array != NULL;
    for (size_t index = 0; built && index < count; index++) {
        cJSON *text = cJSON_CreateString(candidates[index]);
        built = text != NULL && cJSON_AddItemToArray(array, text);
        if (!built) {
            cJSON_Delete(text);
        }
    }
    return built;
}

static cJSON *home_call(const runtime_loader_home_argument *argument) {
    cJSON *call = cJSON_CreateObject();
    cJSON *home = call == NULL ? NULL : cJSON_AddObjectToObject(call, "home");
    cJSON *environment = NULL;
    int built = home != NULL && add_text(home, "envOverride", argument->env_override) &&
                add_text(home, "nativeEnv", argument->native_env) &&
                add_text(home, "xdgSubdir", argument->xdg_subdir) &&
                add_candidates(home, argument->candidates, argument->candidate_count) &&
                runtime_environment_json(argument->environment, argument->environment_count, &environment) ==
                    SPISOR_OK;
    if (built && !cJSON_AddItemToObject(call, "environment", environment)) {
        built = 0;
    }
    if (!built) {
        cJSON_Delete(environment);
    }
    return built_call(call, built);
}

static uint32_t source_of(const char *source) {
    for (uint32_t index = RUNTIME_LOADER_HOME_OVERRIDE; source != NULL && index <= RUNTIME_LOADER_HOME_DEFAULT;
         index++) {
        if (strcmp(source, HOME_SOURCES[index]) == 0) {
            return index;
        }
    }
    return 0;
}

static spisor_status fill_home(const cJSON *answered, void *filled) {
    runtime_loader_home_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status == SPISOR_OK && !runtime_storage_copy(&answer->storage, text_of(answered, "home"), &answer->home)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    answer->source = source_of(text_of(answered, "source"));
    return status;
}

static spisor_status home(void *context, const runtime_loader_home_argument *argument,
                          runtime_loader_home_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, HOME_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, HOME_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "loader's home");
    }
    status = check_home(argument, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_loader_home_operation, home_call(argument), fill_home,
                                                         answer, HOME_ANSWER_SHAPE, refusal);
}

static spisor_status check_installed(const runtime_loader_installed_argument *argument, spisor_refusal *refusal) {
    const runtime_item_version *loader = argument->loader;
    if (!runtime_is_place(argument->cache)) {
        return runtime_refuse_place(refusal, "cache");
    }
    if (loader == NULL || !runtime_are_item_versions(loader, 1) || !runtime_is_plain_host_id(loader->id) ||
        !runtime_is_plain_host_id(loader->version)) {
        return runtime_refuse_member(refusal, "loader", HEADER);
    }
    return SPISOR_OK;
}

static cJSON *installed_call(const runtime_loader_installed_argument *argument) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    return built_call(call, places != NULL && add_text(places, "cache", argument->cache) &&
                                add_text(call, "id", argument->loader->id) &&
                                add_text(call, "version", argument->loader->version));
}

static spisor_status fill_installed(const cJSON *answered, void *filled) {
    runtime_loader_installed_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? runtime_storage_value(&answer->storage,
                                                       cJSON_GetObjectItemCaseSensitive(answered, "standard"),
                                                       &answer->standard)
                               : status;
}

static spisor_status installed(void *context, const runtime_loader_installed_argument *argument,
                               runtime_loader_installed_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, INSTALLED_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, INSTALLED_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "loader's installed");
    }
    status = check_installed(argument, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_loader_installed_operation, installed_call(argument),
                                                         fill_installed, answer, INSTALLED_ANSWER_SHAPE, refusal);
}

static void release_check_answer(void *context, runtime_loader_check_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
}

static void release_home_answer(void *context, runtime_loader_home_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, HOME_ANSWER_SHAPE);
}

static void release_installed_answer(void *context, runtime_loader_installed_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, INSTALLED_ANSWER_SHAPE);
}

static const runtime_loader_definition definition = {
    sizeof(runtime_loader_definition),
    NULL,
    check,
    home,
    installed,
    release_check_answer,
    release_home_answer,
    release_installed_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_loader_definition_of(void) {
    return &definition;
}
