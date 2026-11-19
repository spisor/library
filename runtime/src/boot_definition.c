#include "runtime/boot.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "boot.h"
#include "definition_support.h"
#include "prepared_undo.h"
#include "host_references.h"
#include "json_service.h"
#include "platform_key.h"

#define HEADER "the boot header"
#define HOST_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_host_argument, host_id)
#define HINT_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_hint_answer, storage)
#define WRITE_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_write_argument, platform)
#define RESOLVE_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_resolve_argument, platform)
#define RESOLVE_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_resolve_answer, storage)
#define PLACE_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_place_argument, cache)
#define PLACE_ANSWER_SHAPE SPISOR_SHAPE_THROUGH(runtime_boot_place_answer, storage)

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static spisor_status check_host(const char *cache, const char *host_id, const char *platform,
                                spisor_refusal *refusal) {
    spisor_status status = runtime_is_text(cache) ? SPISOR_OK : runtime_refuse_member(refusal, "cache", HEADER);
    if (status == SPISOR_OK) {
        status = runtime_check_host_id(refusal, host_id, HEADER);
    }
    return status == SPISOR_OK ? runtime_check_platform(refusal, platform, HEADER) : status;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}


static spisor_status copy_text(void **storage, const char *text, const char **copy) {
    return runtime_storage_copy(storage, text, copy) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
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
    spisor_status status = runtime_start_storage(&answer->storage);
    answer->hinted = hint != NULL;
    if (status == SPISOR_OK) {
        status = fill_libraries(&answer->storage, cJSON_GetObjectItemCaseSensitive(hint, "libraries"), answer);
    }
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(hint, "host"), &answer->host) : status;
}

static spisor_status read_hint(void *context, const runtime_boot_host_argument *argument,
                               runtime_boot_hint_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, HOST_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, HINT_ANSWER_SHAPE, answer->library_count)) {
        return spisor_refuse_shape(refusal, "boot's read");
    }
    status = check_host(argument->cache, argument->host_id, NULL, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    answered = runtime_boot_read(argument->cache, argument->host_id, &operation_refusal);
    return runtime_serve_answered(answered, operation_refusal, fill_hint, answer, HINT_ANSWER_SHAPE, refusal);
}

/** The libraries as an object of library id to version; NULL when memory runs out. */
static cJSON *libraries_of(const runtime_boot_write_argument *argument) {
    cJSON *libraries = cJSON_CreateObject();
    int built = libraries != NULL;
    for (size_t index = 0; built && index < argument->library_count; index++) {
        built = add_text(libraries, argument->libraries[index].id, argument->libraries[index].version);
    }
    if (!built) {
        cJSON_Delete(libraries);
        return NULL;
    }
    return libraries;
}

/** Prepares the write, takes the answer's storage, then writes the one file, so a filled answer is never lost. */
static spisor_status serve_write(const runtime_boot_write_argument *argument, runtime_boot_hint_answer *answer,
                                 spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *libraries = libraries_of(argument);
    runtime_boot_hint_request request = {argument->cache, argument->host_id, libraries,
                                         argument->host,  argument->state,   argument->platform};
    runtime_boot_hint_write *write =
        libraries == NULL ? NULL : runtime_boot_prepare_write(&request, &operation_refusal);
    spisor_status status = write == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                         : fill_hint(runtime_boot_write_answer(write), answer);
    if (status == SPISOR_OK && !runtime_boot_commit_write(write, &operation_refusal)) {
        status = runtime_refuse_json(refusal, operation_refusal);
    }
    if (write != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, HINT_ANSWER_SHAPE);
    }
    runtime_boot_free_write(write);
    cJSON_Delete(libraries);
    return status;
}

static spisor_status write_hint(void *context, const runtime_boot_write_argument *argument,
                           runtime_boot_hint_answer *answer, spisor_refusal *refusal) {
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, WRITE_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, HINT_ANSWER_SHAPE, answer->library_count)) {
        return spisor_refuse_shape(refusal, "boot's write");
    }
    status = check_host(argument->cache, argument->host_id, argument->platform, refusal);
    if (status == SPISOR_OK && !runtime_are_item_versions(argument->libraries, argument->library_count)) {
        status = runtime_refuse_member(refusal, "libraries", HEADER);
    }
    if (status == SPISOR_OK && argument->host != NULL && !runtime_is_text(argument->host)) {
        status = runtime_refuse_member(refusal, "host", HEADER);
    }
    if (status == SPISOR_OK && argument->state != NULL && !runtime_is_text(argument->state)) {
        status = runtime_refuse_member(refusal, "state", HEADER);
    }
    return status != SPISOR_OK ? status : serve_write(argument, answer, refusal);
}

static spisor_status fill_resolve(const cJSON *answered, void *filled) {
    runtime_boot_resolve_answer *answer = filled;
    spisor_status status = runtime_start_storage(&answer->storage);
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


static spisor_status resolve_host(void *context, const runtime_boot_resolve_argument *argument,
                             runtime_boot_resolve_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered;
    spisor_status status;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, RESOLVE_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, RESOLVE_ANSWER_SHAPE, answer->argument_count)) {
        return spisor_refuse_shape(refusal, "boot's resolve");
    }
    status = check_host(argument->cache, argument->host_id, argument->platform, refusal);
    if (status == SPISOR_OK && !runtime_are_arguments(argument->arguments, argument->argument_count)) {
        status = runtime_refuse_member(refusal, "arguments", HEADER);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    answered = runtime_boot_resolve(argument->cache, argument->host_id, argument->arguments, argument->argument_count,
                                    argument->platform, &operation_refusal);
    return runtime_serve_answered(answered, operation_refusal, fill_resolve, answer, RESOLVE_ANSWER_SHAPE, refusal);
}

static spisor_status fill_place(const cJSON *answered, runtime_boot_place_answer *answer) {
    spisor_status status = runtime_start_storage(&answer->storage);
    answer->placed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "placed"));
    return status == SPISOR_OK ? copy_text(&answer->storage, text_of(answered, "path"), &answer->path) : status;
}

/** Places spisor-boot, and when the answer cannot be filled removes the copy this call put there. */
static spisor_status serve_place(const char *cache, runtime_boot_place_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *answered = runtime_boot_place(cache, &operation_refusal);
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, operation_refusal)
                                            : fill_place(answered, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, PLACE_ANSWER_SHAPE);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answered, "placed"))) {
            spisor_remove_file_in_place(text_of(answered, "path"));
        }
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status place_stub(void *context, const runtime_boot_place_argument *argument,
                           runtime_boot_place_answer *answer, spisor_refusal *refusal) {
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, PLACE_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, PLACE_ANSWER_SHAPE)) {
        return spisor_refuse_shape(refusal, "boot's place");
    }
    if (!runtime_is_text(argument->cache)) {
        return runtime_refuse_member(refusal, "cache", HEADER);
    }
    return serve_place(argument->cache, answer, refusal);
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
    spisor_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_boot_definition_of(void) {
    return &definition;
}
