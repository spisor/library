#include "runtime/identity.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "identity.h"
#include "identity_also_at.h"
#include "identity_resolve.h"
#include "json_value.h"

#define ID_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_id_argument, id)
#define RESOLVE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_resolve_argument, api_base)
#define AGREE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_agree_argument, copy_count)
#define ALSO_AT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_also_at_argument, api_base)
#define VERSION_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_version_argument, primary)
#define PARTS_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_parts, storage)
#define FOLDER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_folder_answer, storage)
#define REPOSITORY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_repository, storage)
#define AGREEMENT_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_agreement, storage)
#define VERSION_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_identity_version_answer, accepted)

#define ID_FIX "send an id such as github:<number>[/<path>] or local:<name>"
#define REPO_ID_FIX "send a repo id such as github:<number>, with no folder and not local"

static spisor_status refuse_id(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the id is not valid", ID_FIX);
}

static spisor_status refuse_repo_id(spisor_refusal *refusal, const char *what) {
    char detail[96];
    snprintf(detail, sizeof detail, "the %s is not the id of a repo", what);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, REPO_ID_FIX);
}

static int copy_part(void **storage, runtime_identity_part part, const char **copy) {
    char *taken;
    *copy = NULL;
    if (part.start == NULL) {
        return 1;
    }
    taken = runtime_storage_take(storage, part.length + 1);
    if (taken == NULL) {
        return 0;
    }
    memcpy(taken, part.start, part.length);
    *copy = taken;
    return 1;
}

static spisor_status parse(void *context, const runtime_identity_id_argument *argument,
                           runtime_identity_parts *answer, spisor_refusal *refusal) {
    runtime_identity parsed;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ID_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, PARTS_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's parse");
    }
    if (argument->id == NULL) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no id", ID_FIX);
    }
    if (!runtime_identity_parse_id(argument->id, &parsed)) {
        return SPISOR_NONE;
    }
    if (!copy_part(&answer->storage, parsed.resolver, &answer->resolver) ||
        !copy_part(&answer->storage, parsed.host, &answer->host) ||
        !copy_part(&answer->storage, parsed.number, &answer->number) ||
        !copy_part(&answer->storage, parsed.path, &answer->path)) {
        runtime_release_stored_answer(answer, PARTS_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status refuse_folder(runtime_identity_folder_status status, spisor_refusal *refusal) {
    if (status == RUNTIME_IDENTITY_FOLDER_AMBIGUOUS) {
        return runtime_refuse_as(refusal, SPISOR_REFUSED, "the local id would share a folder with another id",
                                 "use the plugin's import name: lowercase, without +, = or :");
    }
    return runtime_refuse_as(refusal, SPISOR_REFUSED, "the id cannot name a folder",
                             "use a local name without path separators, wildcards or a trailing dot");
}

static spisor_status folder(void *context, const runtime_identity_id_argument *argument,
                            runtime_identity_folder_answer *answer, spisor_refusal *refusal) {
    char *name = NULL;
    runtime_identity_folder_status status;
    int copied;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ID_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, FOLDER_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's folder");
    }
    status = argument->id == NULL ? RUNTIME_IDENTITY_FOLDER_INVALID : runtime_identity_folder_name(argument->id, &name);
    if (status == RUNTIME_IDENTITY_FOLDER_INVALID) {
        return refuse_id(refusal);
    }
    if (status == RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (status != RUNTIME_IDENTITY_FOLDER_NAMED) {
        return refuse_folder(status, refusal);
    }
    copied = runtime_storage_copy(&answer->storage, name, &answer->folder);
    spisor_memory_release(name);
    if (!copied) {
        runtime_release_stored_answer(answer, FOLDER_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static cJSON *lookup_argument(const char *id, const char *last_known, const char *token, const char *api_base) {
    cJSON *argument = cJSON_CreateObject();
    if (argument == NULL || !add_text(argument, "id", id) || !add_text(argument, "lastKnown", last_known) ||
        !add_text(argument, "token", token) || !add_text(argument, "apiBase", api_base)) {
        runtime_delete_secret_json(argument);
        return NULL;
    }
    return argument;
}

static const char *text_of(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static spisor_status fill_repository(const cJSON *found, runtime_identity_repository *answer) {
    if (!runtime_storage_copy(&answer->storage, text_of(found, "name"), &answer->name) ||
        !runtime_storage_copy(&answer->storage, text_of(found, "fullName"), &answer->full_name) ||
        !runtime_storage_copy(&answer->storage, text_of(found, "defaultBranch"), &answer->default_branch)) {
        runtime_release_stored_answer(answer, REPOSITORY_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status resolve(void *context, const runtime_identity_resolve_argument *argument,
                             runtime_identity_repository *answer, spisor_refusal *refusal) {
    runtime_identity parsed;
    cJSON *call;
    cJSON *found;
    cJSON *found_refusal = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RESOLVE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, REPOSITORY_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's resolve");
    }
    if (argument->id == NULL || !runtime_identity_parse_id(argument->id, &parsed)) {
        return refuse_id(refusal);
    }
    if (argument->token != NULL && !runtime_identity_is_token_text(argument->token)) {
        return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the token is not usable",
                                 "send token as the access token itself, on one line with no spaces");
    }
    call = lookup_argument(argument->id, argument->last_known, argument->token, argument->api_base);
    if (call == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    found = runtime_identity_lookup(call, &found_refusal);
    runtime_delete_secret_json(call);
    status = found == NULL ? runtime_refuse_json(refusal, found_refusal) : fill_repository(found, answer);
    cJSON_Delete(found);
    return status;
}

static spisor_status refuse_copies(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "a copy carries no id, is listed twice, or holds a file that "
                             "is not a well-formed value",
                             "send each copy once with its id, and its file as read or NULL");
}

static cJSON *file_json(const runtime_identity_copy *copy, spisor_status *status) {
    cJSON *file = NULL;
    *status = SPISOR_OK;
    if (copy->read_failed) {
        file = cJSON_CreateFalse();
    } else if (copy->file == NULL) {
        file = cJSON_CreateNull();
    } else {
        *status = spisor_json_of_value(copy->file, &file);
    }
    if (*status == SPISOR_OK && file == NULL) {
        *status = SPISOR_OUT_OF_MEMORY;
    }
    return file;
}

static spisor_status add_copy(cJSON *files, const runtime_identity_copy *copy) {
    spisor_status status;
    cJSON *file;
    if (copy->id == NULL || cJSON_GetObjectItemCaseSensitive(files, copy->id) != NULL) {
        return SPISOR_MALFORMED;
    }
    file = file_json(copy, &status);
    if (status == SPISOR_OK && !cJSON_AddItemToObject(files, copy->id, file)) {
        cJSON_Delete(file);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

/** The argument runtime_identity_agree_on_files reads, NULL with `*status` saying why. */
static cJSON *agree_json(const runtime_identity_agree_argument *argument, spisor_status *status) {
    cJSON *call = cJSON_CreateObject();
    cJSON *files = call == NULL ? NULL : cJSON_AddObjectToObject(call, "files");
    *status = files != NULL && cJSON_AddStringToObject(call, "id", argument->id) != NULL ? SPISOR_OK
                                                                                         : SPISOR_OUT_OF_MEMORY;
    if (*status == SPISOR_OK && argument->copy_count > 0 && argument->copies == NULL) {
        *status = SPISOR_MALFORMED;
    }
    for (size_t index = 0; *status == SPISOR_OK && index < argument->copy_count; index++) {
        *status = add_copy(files, &argument->copies[index]);
    }
    if (*status != SPISOR_OK) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static spisor_status fill_agreement(const cJSON *found, runtime_identity_agreement *answer) {
    if (!runtime_storage_texts(&answer->storage, cJSON_GetObjectItemCaseSensitive(found, "ids"), &answer->ids,
                               &answer->id_count) ||
        !runtime_storage_copy(&answer->storage, text_of(found, "primary"), &answer->primary)) {
        runtime_release_stored_answer(answer, AGREEMENT_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->primary_confirmed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found, "primaryConfirmed")) ? 1 : 0;
    answer->start_file_malformed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found, "startFileMalformed")) ? 1 : 0;
    answer->start_file_unavailable =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(found, "startFileUnavailable")) ? 1 : 0;
    return SPISOR_OK;
}

typedef cJSON *(*agreement_operation)(const cJSON *argument, cJSON **refusal);

/** Serves `call`, which it deletes, through an operation answering agree's JSON. */
static spisor_status agree_through(agreement_operation operation, cJSON *call, runtime_identity_agreement *answer,
                                   spisor_refusal *refusal) {
    cJSON *found_refusal = NULL;
    cJSON *found = operation(call, &found_refusal);
    spisor_status status = found == NULL ? runtime_refuse_json(refusal, found_refusal) : fill_agreement(found, answer);
    cJSON_Delete(found);
    cJSON_Delete(call);
    return status;
}

static spisor_status agree(void *context, const runtime_identity_agree_argument *argument,
                           runtime_identity_agreement *answer, spisor_refusal *refusal) {
    spisor_status status;
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, AGREE_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, AGREEMENT_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's agree");
    }
    if (!runtime_identity_is_repo_id(argument->id)) {
        return refuse_repo_id(refusal, "id");
    }
    call = agree_json(argument, &status);
    if (status == SPISOR_MALFORMED) {
        return refuse_copies(refusal);
    }
    return call == NULL ? status : agree_through(runtime_identity_agree_on_files, call, answer, refusal);
}

static spisor_status also_at(void *context, const runtime_identity_also_at_argument *argument,
                             runtime_identity_agreement *answer, spisor_refusal *refusal) {
    cJSON *call;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ALSO_AT_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, AGREEMENT_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's also_at");
    }
    if (!runtime_identity_is_repo_id(argument->id)) {
        return refuse_repo_id(refusal, "id");
    }
    call = lookup_argument(argument->id, NULL, NULL, argument->api_base);
    return call == NULL ? SPISOR_OUT_OF_MEMORY
                        : agree_through(runtime_identity_read_also_at, call, answer, refusal);
}

static spisor_status accepts_version(void *context, const runtime_identity_version_argument *argument,
                                     runtime_identity_version_answer *answer, spisor_refusal *refusal) {
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, VERSION_SHAPE) || answer == NULL ||
        !runtime_has_shape(answer->size, VERSION_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "identity's accepts_version");
    }
    if (!runtime_identity_is_repo_id(argument->recorded)) {
        return refuse_repo_id(refusal, "recorded");
    }
    if (!runtime_identity_is_repo_id(argument->primary)) {
        return refuse_repo_id(refusal, "primary");
    }
    answer->accepted = strcmp(argument->recorded, argument->primary) == 0 ? 1 : 0;
    return SPISOR_OK;
}

static void release_parts(void *context, runtime_identity_parts *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PARTS_SHAPE);
}

static void release_folder_answer(void *context, runtime_identity_folder_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, FOLDER_SHAPE);
}

static void release_repository(void *context, runtime_identity_repository *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REPOSITORY_SHAPE);
}

static void release_agreement(void *context, runtime_identity_agreement *answer) {
    (void)context;
    runtime_release_stored_answer(answer, AGREEMENT_SHAPE);
}

static const runtime_identity_definition definition = {
    sizeof(runtime_identity_definition),
    NULL,
    parse,
    folder,
    resolve,
    agree,
    also_at,
    accepts_version,
    release_parts,
    release_folder_answer,
    release_repository,
    release_agreement,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_identity_definition_of(void) {
    return &definition;
}
