#include "runtime/updates.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "host_references.h"
#include "json_service.h"
#include "paths.h"
#include "platform_key.h"
#include "run_records.h"
#include "spisor/path.h"
#include "updates.h"

#define HEADER "the updates header"
#define APPLY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_apply_argument, platform)
#define APPLY_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_apply_answer, storage)
#define CHECK_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_check_argument, platform)
#define CHECK_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_check_answer, storage)
#define ROLLBACK_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_rollback_argument, item)
#define ROLLBACK_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_rollback_answer, storage)
#define RECORDED_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_recorded_argument, host_id)
#define RECORDED_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_recorded_answer, storage)
#define RUN_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_run_argument, now)
#define RUN_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_updates_run_answer, storage)

static const char *const OUTCOMES[] = {NULL, "skipped", "unchanged", "changed", "failed"};
static const char *const OPERATIONS[] = {NULL, "update", "remove"};

/** The host a call is for, as every operation but run names it. */
typedef struct host_target {
    const char *state;
    const char *cache;
    const char *host_id;
    const char *platform;
} host_target;

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static uint32_t index_of(const char *const *names, uint32_t count, const char *name) {
    for (uint32_t index = 1; name != NULL && index < count; index++) {
        if (strcmp(names[index], name) == 0) {
            return index;
        }
    }
    return 0;
}

static spisor_status check_target(const host_target *target, spisor_refusal *refusal) {
    spisor_status status;
    if (!runtime_is_place(target->state) || !runtime_is_place(target->cache)) {
        return runtime_refuse_place(refusal, runtime_is_place(target->state) ? "cache" : "state");
    }
    status = runtime_check_host_id(refusal, target->host_id, HEADER);
    return status == SPISOR_OK ? runtime_check_platform(refusal, target->platform, HEADER) : status;
}

static int are_defaults(const runtime_updates_default *defaults, size_t count) {
    if (count > 0 && defaults == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!runtime_is_text(defaults[index].binding) || !runtime_is_text(defaults[index].plugin)) {
            return 0;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (strcmp(defaults[earlier].binding, defaults[index].binding) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

/** OK for NULL settings or a well-formed record; MALFORMED, refused, for any other; OUT_OF_MEMORY. */
static spisor_status check_settings(const spisor_value *settings, spisor_refusal *refusal) {
    spisor_status status = settings == NULL ? SPISOR_OK : spisor_value_check(settings);
    if (status == SPISOR_OK && settings != NULL && settings->kind != SPISOR_VALUE_RECORD) {
        status = SPISOR_MALFORMED;
    }
    return status == SPISOR_MALFORMED ? runtime_refuse_member(refusal, "settings", HEADER) : status;
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

static int add_versions(cJSON *object, const char *name, const runtime_item_version *versions, size_t count) {
    cJSON *map = cJSON_AddObjectToObject(object, name);
    for (size_t index = 0; map != NULL && index < count; index++) {
        if (!add_text(map, versions[index].id, versions[index].version)) {
            return 0;
        }
    }
    return map != NULL;
}

static int add_defaults(cJSON *object, const runtime_updates_default *defaults, size_t count) {
    cJSON *map = cJSON_AddObjectToObject(object, "defaults");
    for (size_t index = 0; map != NULL && index < count; index++) {
        if (!add_text(map, defaults[index].binding, defaults[index].plugin)) {
            return 0;
        }
    }
    return map != NULL;
}

static int add_damaged(cJSON *object, const runtime_item_version *damaged, size_t count) {
    cJSON *array = cJSON_AddArrayToObject(object, "problems");
    for (size_t index = 0; array != NULL && index < count; index++) {
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL || !add_text(entry, "id", damaged[index].id) ||
            !add_text(entry, "version", damaged[index].version) || !cJSON_AddItemToArray(array, entry)) {
            cJSON_Delete(entry);
            return 0;
        }
    }
    return array != NULL;
}

/** {places: {state, cache}, hostId, platform?}; NULL when memory runs out. */
static cJSON *target_call(const host_target *target) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    if (places == NULL || !add_text(places, "state", target->state) || !add_text(places, "cache", target->cache) ||
        !add_text(call, "hostId", target->host_id) || !add_text(call, "platform", target->platform)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Adds the settings when there are any, deleting `*call` and answering OUT_OF_MEMORY when memory runs out. */
static spisor_status add_settings(cJSON **call, const spisor_value *settings) {
    spisor_status status = *call == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    if (status == SPISOR_OK && settings != NULL) {
        status = runtime_add_value_json(*call, "settings", settings, SPISOR_VALUE_RECORD);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*call);
        *call = NULL;
    }
    return status;
}

static spisor_status fill_versions(void **storage, const cJSON *object, const runtime_item_version **filled,
                                   size_t *count) {
    runtime_item_version *versions;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *versions,
                                    (void **)&versions)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = versions;
    cJSON_ArrayForEach(member, object) {
        runtime_item_version *each = &versions[(*count)++];
        if (!runtime_storage_copy(storage, member->string, &each->id) ||
            !runtime_storage_copy(storage, cJSON_GetStringValue(member), &each->version)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_problems(void **storage, const cJSON *array, const runtime_updates_problem **filled,
                                   size_t *count) {
    runtime_updates_problem *problems;
    const cJSON *entry;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), sizeof *problems,
                                    (void **)&problems)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = problems;
    cJSON_ArrayForEach(entry, array) {
        runtime_updates_problem *each = &problems[(*count)++];
        if (!runtime_storage_copy(storage, text_of(entry, "id"), &each->id) ||
            !runtime_storage_copy(storage, text_of(entry, "message"), &each->detail) ||
            !runtime_storage_copy(storage, text_of(entry, "fix"), &each->fix)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_sources(void **storage, const cJSON *array, const runtime_updates_source_check **filled,
                                  size_t *count) {
    runtime_updates_source_check *sources;
    const cJSON *entry;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(array), sizeof *sources,
                                    (void **)&sources)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = sources;
    cJSON_ArrayForEach(entry, array) {
        runtime_updates_source_check *each = &sources[(*count)++];
        each->outcome = index_of(OUTCOMES, 5, text_of(entry, "kind"));
        if (!runtime_storage_copy(storage, text_of(entry, "detail"), &each->detail)) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_texts(void **storage, const cJSON *array, const char *const **texts, size_t *count) {
    return runtime_storage_texts(storage, array, texts, count) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static const cJSON *member_of(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

/** The references run, next and rollback of `answered` into the three arrays given. */
static spisor_status fill_references(void **storage, const cJSON *answered, runtime_item_version const **arrays[3],
                                     size_t *counts[3]) {
    static const char *const names[3] = {"run", "next", "rollback"};
    spisor_status status = SPISOR_OK;
    for (size_t index = 0; status == SPISOR_OK && index < 3; index++) {
        status = fill_versions(storage, member_of(answered, names[index]), arrays[index], counts[index]);
    }
    return status;
}

spisor_status runtime_fill_apply_plan(void **storage, const cJSON *answered, runtime_updates_apply_answer *answer) {
    runtime_item_version const **arrays[3] = {&answer->run, &answer->next, &answer->rollback};
    size_t *counts[3] = {&answer->run_count, &answer->next_count, &answer->rollback_count};
    spisor_status status = fill_versions(storage, member_of(answered, "plugins"), &answer->plugins,
                                         &answer->plugin_count);
    if (status == SPISOR_OK) {
        status = fill_references(storage, answered, arrays, counts);
    }
    if (status == SPISOR_OK) {
        status = fill_problems(storage, member_of(answered, "unrecorded"), &answer->unrecorded,
                               &answer->unrecorded_count);
    }
    if (status == SPISOR_OK && !runtime_storage_copy(storage, text_of(answered, "runRecord"), &answer->run_record)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static spisor_status fill_apply(const cJSON *answered, void *filled) {
    runtime_updates_apply_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? runtime_fill_apply_plan(&answer->storage, answered, answer) : status;
}

static spisor_status check_apply(const runtime_updates_apply_argument *argument, spisor_refusal *refusal) {
    host_target target = {argument->state, argument->cache, argument->host_id, argument->platform};
    spisor_status status = check_target(&target, refusal);
    if (status == SPISOR_OK && !are_defaults(argument->defaults, argument->default_count)) {
        status = runtime_refuse_member(refusal, "defaults", HEADER);
    }
    if (status == SPISOR_OK && !runtime_are_item_versions(argument->running, argument->running_count)) {
        status = runtime_refuse_member(refusal, "running", HEADER);
    }
    if (status == SPISOR_OK && (argument->writes_boot_hint > 1 ||
                                (!argument->writes_boot_hint && argument->library_count > 0) ||
                                !runtime_are_texts(argument->libraries, argument->library_count, 1))) {
        status = runtime_refuse_member(refusal, "libraries", HEADER);
    }
    return status == SPISOR_OK ? check_settings(argument->settings, refusal) : status;
}

static cJSON *apply_call(const runtime_updates_apply_argument *argument) {
    host_target target = {argument->state, argument->cache, argument->host_id, argument->platform};
    cJSON *call = target_call(&target);
    int built = call != NULL &&
                add_defaults(call, argument->defaults, argument->default_count) &&
                add_versions(call, "running", argument->running, argument->running_count) &&
                (!argument->writes_boot_hint ||
                 add_texts(call, "libraries", argument->libraries, argument->library_count));
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return add_settings(&call, argument->settings) == SPISOR_OK ? call : NULL;
}

/** Applies, and when the answer cannot be filled removes the run record the apply wrote last. */
static spisor_status serve_apply(const runtime_updates_apply_argument *argument, cJSON *call,
                                 runtime_updates_apply_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_call_operation(runtime_updates_apply_operation, call, &operation_refusal);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                            : fill_apply(answered, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, APPLY_ANSWER_SHAPE);
        runtime_remove_run_record(argument->state, argument->host_id, text_of(answered, "runRecord"));
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status apply(void *context, const runtime_updates_apply_argument *argument,
                           runtime_updates_apply_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, APPLY_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, APPLY_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "updates' apply");
    }
    status = check_apply(argument, refusal);
    return status != SPISOR_OK ? status : serve_apply(argument, apply_call(argument), answer, refusal);
}

/** A check's sources, fetched, refused and removed into the four arrays of a check or a host's check. */
typedef struct check_parts {
    const runtime_updates_source_check **sources;
    size_t *source_count;
    const char *const **fetched;
    size_t *fetched_count;
    const runtime_updates_problem **refused;
    size_t *refused_count;
    const char *const **removed;
    size_t *removed_count;
} check_parts;

static spisor_status fill_check_parts(void **storage, const cJSON *answered, const check_parts *parts) {
    spisor_status status = fill_sources(storage, member_of(answered, "checked"), parts->sources, parts->source_count);
    if (status == SPISOR_OK) {
        status = fill_texts(storage, member_of(answered, "fetched"), parts->fetched, parts->fetched_count);
    }
    if (status == SPISOR_OK) {
        status = fill_problems(storage, member_of(answered, "refused"), parts->refused, parts->refused_count);
    }
    return status == SPISOR_OK
               ? fill_texts(storage, member_of(answered, "removed"), parts->removed, parts->removed_count)
               : status;
}

static spisor_status fill_check(const cJSON *answered, void *filled) {
    runtime_updates_check_answer *answer = filled;
    check_parts parts = {&answer->sources, &answer->source_count, &answer->fetched, &answer->fetched_count,
                         &answer->refused, &answer->refused_count, &answer->removed, &answer->removed_count};
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? fill_check_parts(&answer->storage, answered, &parts) : status;
}

static spisor_status check_check(const runtime_updates_check_argument *argument, spisor_refusal *refusal) {
    host_target target = {argument->state, argument->cache, argument->host_id, argument->platform};
    spisor_status status = check_target(&target, refusal);
    if (status == SPISOR_OK && !runtime_is_text(argument->now)) {
        status = runtime_refuse_member(refusal, "now", HEADER);
    }
    if (status == SPISOR_OK && !are_defaults(argument->defaults, argument->default_count)) {
        status = runtime_refuse_member(refusal, "defaults", HEADER);
    }
    if (status == SPISOR_OK && !runtime_are_item_versions(argument->damaged, argument->damaged_count)) {
        status = runtime_refuse_member(refusal, "damaged", HEADER);
    }
    return status == SPISOR_OK ? check_settings(argument->settings, refusal) : status;
}

static cJSON *check_call(const runtime_updates_check_argument *argument) {
    host_target target = {argument->state, argument->cache, argument->host_id, argument->platform};
    cJSON *call = target_call(&target);
    int built = call != NULL && add_text(call, "now", argument->now) &&
                add_defaults(call, argument->defaults, argument->default_count) &&
                add_damaged(call, argument->damaged, argument->damaged_count);
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return add_settings(&call, argument->settings) == SPISOR_OK ? call : NULL;
}

static spisor_status check(void *context, const runtime_updates_check_argument *argument,
                           runtime_updates_check_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CHECK_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, CHECK_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "updates' check");
    }
    status = check_check(argument, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_updates_check_operation, check_call(argument),
                                                         fill_check, answer, CHECK_ANSWER_SHAPE, refusal);
}

static spisor_status fill_rollback(const cJSON *answered, void *filled) {
    runtime_updates_rollback_answer *answer = filled;
    runtime_item_version const **arrays[3] = {&answer->run, &answer->next, &answer->rollback};
    size_t *counts[3] = {&answer->run_count, &answer->next_count, &answer->rollback_count};
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK ? fill_references(&answer->storage, answered, arrays, counts) : status;
}

static int fill_rollback_before_commit(const cJSON *answered, void *filled) {
    return fill_rollback(answered, filled) == SPISOR_OK;
}

/** Serves `call`, which it deletes, filling the answer before the rollback is recorded. */
static spisor_status serve_rollback(cJSON *call, runtime_updates_rollback_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = call == NULL ? NULL
                                   : runtime_updates_rollback_filling(call, fill_rollback_before_commit, answer,
                                                                      &operation_refusal);
    cJSON_Delete(call);
    if (answered == NULL) {
        runtime_release_stored_answer(answer, ROLLBACK_ANSWER_SHAPE);
        return runtime_refuse_json(refusal, operation_refusal);
    }
    cJSON_Delete(answered);
    return SPISOR_OK;
}

static spisor_status rollback(void *context, const runtime_updates_rollback_argument *argument,
                              runtime_updates_rollback_answer *answer, spisor_refusal *refusal) {
    host_target target;
    cJSON *call;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ROLLBACK_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, ROLLBACK_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "updates' rollback");
    }
    target = (host_target){argument->state, argument->cache, argument->host_id, NULL};
    status = check_target(&target, refusal);
    if (status == SPISOR_OK && !runtime_is_text(argument->item)) {
        status = runtime_refuse_member(refusal, "item", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    call = target_call(&target);
    if (call != NULL && !add_text(call, "item", argument->item)) {
        cJSON_Delete(call);
        call = NULL;
    }
    return serve_rollback(call, answer, refusal);
}

static spisor_status fill_recorded(const cJSON *answered, void *filled) {
    runtime_updates_recorded_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    return status == SPISOR_OK
               ? fill_versions(&answer->storage, member_of(answered, "run"), &answer->run, &answer->run_count)
               : status;
}

static spisor_status recorded(void *context, const runtime_updates_recorded_argument *argument,
                              runtime_updates_recorded_answer *answer, spisor_refusal *refusal) {
    host_target target;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RECORDED_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RECORDED_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "updates' recorded");
    }
    target = (host_target){argument->state, argument->cache, argument->host_id, NULL};
    status = check_target(&target, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_updates_recorded_operation, target_call(&target),
                                                         fill_recorded, answer, RECORDED_ANSWER_SHAPE, refusal);
}

static spisor_status fill_host_checks(void **storage, const cJSON *object, const runtime_updates_host_check **filled,
                                      size_t *count) {
    runtime_updates_host_check *checks;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *checks, (void **)&checks)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = checks;
    cJSON_ArrayForEach(member, object) {
        runtime_updates_host_check *each = &checks[(*count)++];
        check_parts parts = {&each->sources, &each->source_count, &each->fetched, &each->fetched_count,
                             &each->refused, &each->refused_count, &each->removed, &each->removed_count};
        if (!runtime_storage_copy(storage, member->string, &each->host_id) ||
            fill_check_parts(storage, member, &parts) != SPISOR_OK) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_application(void **storage, const cJSON *member, runtime_updates_application *each) {
    const cJSON *unrecorded = member_of(member, "unrecorded");
    each->operation = index_of(OPERATIONS, 3, text_of(member, "operation"));
    return runtime_storage_copy(storage, member->string, &each->loader) &&
                   runtime_storage_copy(storage, text_of(member, "version"), &each->version) &&
                   runtime_storage_copy(storage, text_of(unrecorded, "message"), &each->unrecorded_detail) &&
                   runtime_storage_copy(storage, text_of(unrecorded, "fix"), &each->unrecorded_fix)
               ? SPISOR_OK
               : SPISOR_OUT_OF_MEMORY;
}

static spisor_status fill_applications(void **storage, const cJSON *object,
                                       const runtime_updates_application **filled, size_t *count) {
    runtime_updates_application *applications;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *applications,
                                    (void **)&applications)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *filled = applications;
    cJSON_ArrayForEach(member, object) {
        if (fill_application(storage, member, &applications[(*count)++]) != SPISOR_OK) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_run(const cJSON *answered, void *filled) {
    runtime_updates_run_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = fill_host_checks(&answer->storage, member_of(answered, "checks"), &answer->checks,
                                  &answer->check_count);
    }
    if (status == SPISOR_OK) {
        status = fill_applications(&answer->storage, member_of(answered, "applications"), &answer->applications,
                                   &answer->application_count);
    }
    return status == SPISOR_OK ? fill_problems(&answer->storage, member_of(answered, "refused"), &answer->refused,
                                               &answer->refused_count)
                               : status;
}

static cJSON *run_call(const runtime_updates_run_argument *argument) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = call == NULL ? NULL : cJSON_AddObjectToObject(call, "places");
    if (places == NULL || !add_text(places, "portable", argument->portable) ||
        !add_text(places, "state", argument->state) || !add_text(places, "cache", argument->cache) ||
        !add_text(call, "now", argument->now)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status run(void *context, const runtime_updates_run_argument *argument,
                         runtime_updates_run_answer *answer, spisor_refusal *refusal) {
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RUN_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RUN_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "updates' run");
    }
    if (!runtime_is_place(argument->portable) || !runtime_is_place(argument->state) ||
        !runtime_is_place(argument->cache)) {
        return runtime_refuse_place(refusal, !runtime_is_place(argument->portable) ? "portable"
                                             : !runtime_is_place(argument->state)  ? "state"
                                                                                   : "cache");
    }
    if (!runtime_is_text(argument->now)) {
        return runtime_refuse_member(refusal, "now", HEADER);
    }
    return runtime_serve_operation(runtime_updates_run_operation, run_call(argument), fill_run, answer,
                                   RUN_ANSWER_SHAPE, refusal);
}

static void release_apply_answer(void *context, runtime_updates_apply_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, APPLY_ANSWER_SHAPE);
}

static void release_check_answer(void *context, runtime_updates_check_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, CHECK_ANSWER_SHAPE);
}

static void release_rollback_answer(void *context, runtime_updates_rollback_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, ROLLBACK_ANSWER_SHAPE);
}

static void release_recorded_answer(void *context, runtime_updates_recorded_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RECORDED_ANSWER_SHAPE);
}

static void release_run_answer(void *context, runtime_updates_run_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RUN_ANSWER_SHAPE);
}

static const runtime_updates_definition definition = {
    sizeof(runtime_updates_definition),
    NULL,
    apply,
    check,
    rollback,
    recorded,
    run,
    release_apply_answer,
    release_check_answer,
    release_rollback_answer,
    release_recorded_answer,
    release_run_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_updates_definition_of(void) {
    return &definition;
}
