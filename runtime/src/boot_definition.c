#include "runtime/boot.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "boot.h"
#include "definition_support.h"
#include "directory_tree.h"
#include "host_references.h"
#include "json_service.h"
#include "platform_key.h"

#define HOST_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_host_argument, host_id)
#define HINT_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_hint_answer, storage)
#define WRITE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_write_argument, platform)
#define RESOLVE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_resolve_argument, platform)
#define RESOLVE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_resolve_answer, storage)
#define PLACE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_place_argument, cache)
#define PLACE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_boot_place_answer, storage)

static int is_text(const char *text) {
    return text != NULL && text[0] != '\0';
}

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static spisor_status refuse_malformed(spisor_refusal *refusal, const char *member) {
    char detail[160];
    char fix[200];
    snprintf(detail, sizeof detail, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as the boot header describes it", member);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static spisor_status check_host(const char *cache, const char *host_id, const char *platform,
                                spisor_refusal *refusal) {
    if (!is_text(cache)) {
        return refuse_malformed(refusal, "cache");
    }
    if (!is_text(host_id)) {
        return refuse_malformed(refusal, "host_id");
    }
    if (!runtime_is_plain_host_id(host_id)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the host id is not a plain file name",
                                 RUNTIME_PLAIN_HOST_ID_FIX);
    }
    return platform != NULL && !runtime_is_platform_key(platform) ? refuse_malformed(refusal, "platform") : SPISOR_OK;
}

static int are_versions(const runtime_boot_version *versions, size_t count) {
    if (count > 0 && versions == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_text(versions[index].id) || !is_text(versions[index].version)) {
            return 0;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (strcmp(versions[earlier].id, versions[index].id) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int are_arguments(const char *const *arguments, size_t count) {
    if (count > 0 && arguments == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (arguments[index] == NULL) {
            return 0;
        }
    }
    return 1;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

/** {cache, hostId, platform?}; NULL when memory runs out. */
static cJSON *host_call(const char *cache, const char *host_id, const char *platform) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !add_text(call, "cache", cache) || !add_text(call, "hostId", host_id) ||
        !add_text(call, "platform", platform)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status copy_text(void **storage, const char *text, const char **copy) {
    return runtime_storage_copy(storage, text, copy) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status start_storage(void **storage) {
    return runtime_storage_take(storage, 1) == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status fill_libraries(void **storage, const cJSON *object, runtime_boot_hint_answer *answer) {
    runtime_boot_library *libraries;
    const cJSON *member;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(object), sizeof *libraries,
                                    (void **)&libraries)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->libraries = libraries;
    cJSON_ArrayForEach(member, object) {
        runtime_boot_library *each = &libraries[answer->library_count++];
        if (copy_text(storage, member->string, &each->id) != SPISOR_OK ||
            copy_text(storage, text_of(member, "version"), &each->version) != SPISOR_OK ||
            copy_text(storage, text_of(member, "directory"), &each->directory) != SPISOR_OK ||
            copy_text(storage, text_of(member, "binary"), &each->binary) != SPISOR_OK) {
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return SPISOR_OK;
}

static spisor_status fill_hint(const cJSON *answered, void *filled) {
    runtime_boot_hint_answer *answer = filled;
    const cJSON *hint = cJSON_GetObjectItemCaseSensitive(answered, "hint");
    spisor_status status = start_storage(&answer->storage);
    answer->hinted = hint != NULL;
    if (status == SPISOR_OK) {
        status = fill_libraries(&answer->storage, cJSON_GetObjectItemCaseSensitive(hint, "libraries"), answer);
    }
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(hint, "host"), &answer->host) : status;
}

static spisor_status read_hint(void *context, const runtime_boot_host_argument *argument,
                               runtime_boot_hint_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, HOST_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, HINT_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "boot's read");
    }
    status = check_host(argument->cache, argument->host_id, NULL, refusal);
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_boot_read_operation,
                                                         host_call(argument->cache, argument->host_id, NULL),
                                                         fill_hint, answer, HINT_ANSWER_SHAPE, refusal);
}

static cJSON *write_call(const runtime_boot_write_argument *argument) {
    cJSON *call = host_call(argument->cache, argument->host_id, argument->platform);
    cJSON *libraries = call == NULL ? NULL : cJSON_AddObjectToObject(call, "libraries");
    int built = libraries != NULL && add_text(call, "host", argument->host) && add_text(call, "state", argument->state);
    for (size_t index = 0; built && index < argument->library_count; index++) {
        built = add_text(libraries, argument->libraries[index].id, argument->libraries[index].version);
    }
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Prepares the write, takes the answer's storage, then writes the one file, so a filled answer is never lost. */
static spisor_status serve_write(cJSON *call, runtime_boot_hint_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    runtime_boot_hint_write *write = call == NULL ? NULL : runtime_boot_prepare_write(call, &operation_refusal);
    spisor_status status = write == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                         : fill_hint(runtime_boot_write_answer(write), answer);
    if (status == SPISOR_OK && !runtime_boot_commit_write(write, &operation_refusal)) {
        status = runtime_refuse_json(refusal, operation_refusal);
    }
    if (write != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, HINT_ANSWER_SHAPE);
    }
    runtime_boot_free_write(write);
    cJSON_Delete(call);
    return status;
}

static spisor_status write_hint(void *context, const runtime_boot_write_argument *argument,
                           runtime_boot_hint_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, WRITE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, HINT_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "boot's write");
    }
    status = check_host(argument->cache, argument->host_id, argument->platform, refusal);
    if (status == SPISOR_OK && !are_versions(argument->libraries, argument->library_count)) {
        status = refuse_malformed(refusal, "libraries");
    }
    if (status == SPISOR_OK && argument->host != NULL && !is_text(argument->host)) {
        status = refuse_malformed(refusal, "host");
    }
    if (status == SPISOR_OK && argument->state != NULL && !is_text(argument->state)) {
        status = refuse_malformed(refusal, "state");
    }
    return status != SPISOR_OK ? status : serve_write(write_call(argument), answer, refusal);
}

static spisor_status fill_resolve(const cJSON *answered, void *filled) {
    runtime_boot_resolve_answer *answer = filled;
    spisor_status status = start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = copy_text(&answer->storage, text_of(answered, "executable"), &answer->executable);
    }
    if (status == SPISOR_OK) {
        status = copy_text(&answer->storage, text_of(answered, "version"), &answer->version);
    }
    if (status == SPISOR_OK) {
        status = copy_text(&answer->storage, text_of(answered, "directory"), &answer->directory);
    }
    if (status == SPISOR_OK &&
        !runtime_storage_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(answered, "arguments"),
                               &answer->arguments, &answer->argument_count)) {
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static cJSON *resolve_call(const runtime_boot_resolve_argument *argument) {
    cJSON *call = host_call(argument->cache, argument->host_id, argument->platform);
    cJSON *arguments = call == NULL ? NULL : cJSON_AddArrayToObject(call, "arguments");
    for (size_t index = 0; arguments != NULL && index < argument->argument_count; index++) {
        cJSON *text = cJSON_CreateString(argument->arguments[index]);
        if (text == NULL || !cJSON_AddItemToArray(arguments, text)) {
            cJSON_Delete(text);
            arguments = NULL;
        }
    }
    if (arguments == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status resolve_host(void *context, const runtime_boot_resolve_argument *argument,
                             runtime_boot_resolve_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RESOLVE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, RESOLVE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "boot's resolve");
    }
    status = check_host(argument->cache, argument->host_id, argument->platform, refusal);
    if (status == SPISOR_OK && !are_arguments(argument->arguments, argument->argument_count)) {
        status = refuse_malformed(refusal, "arguments");
    }
    return status != SPISOR_OK ? status
                               : runtime_serve_operation(runtime_boot_resolve_operation, resolve_call(argument),
                                                         fill_resolve, answer, RESOLVE_ANSWER_SHAPE, refusal);
}

static spisor_status fill_place(const cJSON *answered, runtime_boot_place_answer *answer) {
    spisor_status status = start_storage(&answer->storage);
    answer->placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "placed"));
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(answered, "path"), &answer->path) : status;
}

/** Places spisor-boot, and when the answer cannot be filled removes the copy this call put there. */
static spisor_status serve_place(cJSON *call, runtime_boot_place_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_call_operation(runtime_boot_place_operation, call, &operation_refusal);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                            : fill_place(answered, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, PLACE_ANSWER_SHAPE);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "placed"))) {
            runtime_remove_file_entry(text_of(answered, "path"));
        }
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status place_stub(void *context, const runtime_boot_place_argument *argument,
                           runtime_boot_place_answer *answer, spisor_refusal *refusal) {
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, PLACE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, PLACE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "boot's place");
    }
    if (!is_text(argument->cache)) {
        return refuse_malformed(refusal, "cache");
    }
    call = cJSON_CreateObject();
    if (call != NULL && !add_text(call, "cache", argument->cache)) {
        cJSON_Delete(call);
        call = NULL;
    }
    return serve_place(call, answer, refusal);
}

static void release_hint_answer(void *context, runtime_boot_hint_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, HINT_ANSWER_SHAPE);
}

static void release_resolve_answer(void *context, runtime_boot_resolve_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RESOLVE_ANSWER_SHAPE);
}

static void release_place_answer(void *context, runtime_boot_place_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PLACE_ANSWER_SHAPE);
}

static const runtime_boot_definition definition = {
    sizeof(runtime_boot_definition),
    NULL,
    read_hint,
    write_hint,
    resolve_host,
    place_stub,
    release_hint_answer,
    release_resolve_answer,
    release_place_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_boot_definition_of(void) {
    return &definition;
}
