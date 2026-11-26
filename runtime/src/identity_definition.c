#include "runtime/identity.h"
#include "allocation.h"

#include <stdio.h>
#include <string.h>

#include "answer_storage.h"
#include "definition_support.h"
#include "identity.h"
#include "identity_naula.h"
#include "naula/id.h"

#define ID_SHAPE SPISOR_SHAPE_THROUGH(runtime_identity_id_argument, id)
#define RESOLVE_SHAPE SPISOR_SHAPE_THROUGH(runtime_identity_resolve_argument, accept_number)
#define PARTS_SHAPE SPISOR_SHAPE_THROUGH(runtime_identity_parts, storage)
#define FOLDER_SHAPE SPISOR_SHAPE_THROUGH(runtime_identity_folder_answer, storage)
#define RESOLUTION_SHAPE SPISOR_SHAPE_THROUGH(runtime_identity_resolution, storage)

#define ID_FIX "send an id such as git:<root commit>[/<path>] or local:<name>"
#define REPO_ID_FIX "send a repo id such as git:<root commit>, with no folder and not local"
#define TOKEN_FIX "send token as the access token itself, on one line with no spaces, with the token_host it belongs to"
#define MESSAGE_SIZE 1400

static spisor_status refuse_id(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the id is not valid", ID_FIX);
}

static spisor_status copy_segments(void **storage, const naula_id *id, const char **path) {
    size_t length = 0;
    *path = NULL;
    if (id->segment_count == 0) {
        return SPISOR_OK;
    }
    for (size_t index = 0; index < id->segment_count; index++) {
        length += strlen(id->segments[index]) + 1;
    }
    char *joined = runtime_storage_take(storage, length);
    if (joined == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    for (size_t index = 0; index < id->segment_count; index++) {
        strcat(joined, id->segments[index]);
        if (index + 1 < id->segment_count) {
            strcat(joined, "/");
        }
    }
    *path = joined;
    return SPISOR_OK;
}

static spisor_status fill_parts(const naula_id *id, runtime_identity_parts *answer) {
    int is_git = id->kind == NAULA_ID_GIT;
    if (!runtime_storage_copy(&answer->storage, is_git ? "git" : "local", &answer->kind) ||
        !runtime_storage_copy(&answer->storage, is_git ? id->commit : NULL, &answer->commit) ||
        !runtime_storage_copy(&answer->storage, id->local_name, &answer->name) ||
        copy_segments(&answer->storage, id, &answer->path) != SPISOR_OK) {
        runtime_release_stored_answer(answer, PARTS_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status parse(void *context, const runtime_identity_id_argument *argument,
                           runtime_identity_parts *answer, spisor_refusal *refusal) {
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ID_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, PARTS_SHAPE)) {
        return spisor_refuse_shape(refusal, "identity's parse");
    }
    if (argument->id == NULL) {
        return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no id", ID_FIX);
    }
    naula_id id;
    memset(&id, 0, sizeof id);
    id.size = sizeof id;
    naula_status status = naula_id_parse(argument->id, strlen(argument->id), &id, NULL);
    if (status == NAULA_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (status != NAULA_OK) {
        return SPISOR_NONE;
    }
    spisor_status filled = fill_parts(&id, answer);
    naula_id_release(&id);
    return filled;
}

static spisor_status refuse_folder(runtime_identity_folder_status status, spisor_refusal *refusal) {
    if (status == RUNTIME_IDENTITY_FOLDER_AMBIGUOUS) {
        return spisor_refuse_as(refusal, SPISOR_REFUSED, "the local id would share a folder with another id",
                                 "use the plugin's import name: lowercase, without +, = or :");
    }
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "the id cannot name a folder",
                             "use a local name without path separators, wildcards or a trailing dot");
}

static spisor_status folder(void *context, const runtime_identity_id_argument *argument,
                            runtime_identity_folder_answer *answer, spisor_refusal *refusal) {
    char *name = NULL;
    runtime_identity_folder_status status;
    int copied;
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, ID_SHAPE) ||
        !SPISOR_IS_EMPTY_ANSWER(answer, FOLDER_SHAPE)) {
        return spisor_refuse_shape(refusal, "identity's folder");
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

static spisor_status check_repo_id(const char *id, spisor_refusal *refusal) {
    runtime_identity_kind kind = id == NULL ? RUNTIME_IDENTITY_NOT_AN_ID : runtime_identity_kind_of(id);
    if (kind == RUNTIME_IDENTITY_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (kind != RUNTIME_IDENTITY_GIT || !runtime_identity_names_a_whole_repo(id)) {
        return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the id is not the id of a repo", REPO_ID_FIX);
    }
    return SPISOR_OK;
}

static spisor_status refuse_naula(naula_status status, const naula_refusal *naula_refused,
                                  spisor_refusal *refusal) {
    char message[MESSAGE_SIZE];
    if (status == NAULA_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    runtime_identity_refusal_text(naula_refused, message, sizeof message);
    return spisor_refuse_as(refusal, status == NAULA_MALFORMED ? SPISOR_MALFORMED : SPISOR_REFUSED, message,
                            runtime_identity_refusal_fix(naula_refused));
}

static const char *state_text(uint32_t state) {
    return state == NAULA_STALE ? "stale" : state == NAULA_DOMAIN_ONLY ? "domain-only" : "confirmed";
}

static int copy_place(void **storage, const naula_place *place, const char **copy) {
    char text[RUNTIME_PLACE_TEXT_SIZE];
    return runtime_identity_place_text(place, text, sizeof text) && runtime_storage_copy(storage, text, copy);
}

static int copy_full_name(void **storage, const naula_repository *repository, const char **copy) {
    char text[RUNTIME_PLACE_TEXT_SIZE];
    *copy = NULL;
    if (repository->account == NULL || repository->name == NULL) {
        return 1;
    }
    snprintf(text, sizeof text, "%s/%s", repository->account, repository->name);
    return runtime_storage_copy(storage, text, copy);
}

static int fill_homes(void **storage, const naula_resolution *found, runtime_identity_resolution *answer) {
    const char **copies = NULL;
    if (!runtime_storage_take_array(storage, found->home_count, sizeof *copies, (void **)&copies)) {
        return 0;
    }
    for (size_t index = 0; index < found->home_count; index++) {
        if (!copy_place(storage, &found->homes[index], &copies[index])) {
            return 0;
        }
    }
    answer->homes = copies;
    answer->home_count = found->home_count;
    return 1;
}

static int fill_found(const naula_resolution *found, runtime_identity_resolution *answer) {
    const naula_repository *primary = &found->primary_repository;
    void **storage = &answer->storage;
    return runtime_storage_copy(storage, state_text(found->state), &answer->state) &&
           fill_homes(storage, found, answer) &&
           (found->home_count == 0 || copy_place(storage, &found->homes[found->primary], &answer->primary)) &&
           copy_full_name(storage, primary, &answer->full_name) &&
           runtime_storage_copy(storage, primary->name, &answer->name) &&
           runtime_storage_copy(storage, primary->default_branch, &answer->default_branch);
}

static int fill_asked(const runtime_identity_acceptance *acceptance, runtime_identity_resolution *answer) {
    void **storage = &answer->storage;
    if (acceptance->primary_needed) {
        answer->needs_user_acceptance = 1;
        return runtime_storage_copy(storage, acceptance->recorded_primary, &answer->recorded_primary) &&
               runtime_storage_copy(storage, acceptance->offered_primary, &answer->primary);
    }
    answer->needs_number_acceptance = 1;
    runtime_identity_dispute *disputes = NULL;
    if (!runtime_storage_take_array(storage, acceptance->dispute_count, sizeof *disputes, (void **)&disputes)) {
        return 0;
    }
    for (size_t index = 0; index < acceptance->dispute_count; index++) {
        if (!runtime_storage_copy(storage, acceptance->disputes[index].account, &disputes[index].account) ||
            !runtime_storage_copy(storage, acceptance->disputes[index].recorded_number, &disputes[index].recorded_number) ||
            !runtime_storage_copy(storage, acceptance->disputes[index].offered_number, &disputes[index].offered_number) ||
            !runtime_storage_copy(storage, acceptance->disputes[index].accept, &disputes[index].accept)) {
            return 0;
        }
    }
    answer->disputes = disputes;
    answer->dispute_count = acceptance->dispute_count;
    return 1;
}

static spisor_status check_resolve_argument(const runtime_identity_resolve_argument *argument,
                                  spisor_refusal *refusal) {
    spisor_status status = check_repo_id(argument->id, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    if ((argument->token == NULL) != (argument->token_host == NULL) ||
        (argument->token != NULL && !runtime_identity_is_token_text(argument->token))) {
        return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the token is not usable", TOKEN_FIX);
    }
    if (argument->cache != NULL && !runtime_is_place(argument->cache)) {
        return runtime_refuse_member(refusal, "cache", "runtime/identity.h");
    }
    return SPISOR_OK;
}

static spisor_status fill_resolution(const naula_resolution *found, const runtime_identity_acceptance *acceptance,
                                     int asked, runtime_identity_resolution *answer) {
    int filled = asked ? fill_asked(acceptance, answer) : fill_found(found, answer);
    if (!filled) {
        runtime_release_stored_answer(answer, RESOLUTION_SHAPE);
        return SPISOR_OUT_OF_MEMORY;
    }
    return SPISOR_OK;
}

static spisor_status resolve(void *context, const runtime_identity_resolve_argument *argument,
                             runtime_identity_resolution *answer, spisor_refusal *refusal) {
    (void)context;
    if (argument == NULL || !spisor_has_shape(argument->size, RESOLVE_SHAPE) ||
        !SPISOR_IS_EMPTY_COUNTED_ANSWER(answer, RESOLUTION_SHAPE, answer->home_count | answer->dispute_count)) {
        return spisor_refuse_shape(refusal, "identity's resolve");
    }
    spisor_status status = check_resolve_argument(argument, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    runtime_identity_request request = {argument->id,    argument->at,         argument->at_count,
                                        argument->cache, argument->token,      argument->token_host,
                                        argument->accept_primary, argument->accept_number};
    naula_resolution found;
    memset(&found, 0, sizeof found);
    found.size = sizeof found;
    runtime_identity_acceptance acceptance;
    naula_refusal naula_refused;
    memset(&naula_refused, 0, sizeof naula_refused);
    naula_refused.size = sizeof naula_refused;
    naula_status resolved = runtime_identity_resolve(&request, &found, &acceptance, &naula_refused);
    int asked = runtime_identity_needs_the_user(resolved, &naula_refused, &acceptance);
    if (resolved != NAULA_OK && !asked) {
        naula_resolution_release(&found);
        return refuse_naula(resolved, &naula_refused, refusal);
    }
    status = runtime_start_storage(&answer->storage);
    if (status == SPISOR_OK) {
        status = fill_resolution(&found, &acceptance, asked, answer);
    }
    naula_resolution_release(&found);
    return status;
}

static void release_parts(void *context, runtime_identity_parts *answer) {
    (void)context;
    runtime_release_stored_answer(answer, PARTS_SHAPE);
}

static void release_folder_answer(void *context, runtime_identity_folder_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, FOLDER_SHAPE);
}

static void release_resolution(void *context, runtime_identity_resolution *answer) {
    (void)context;
    runtime_release_stored_answer(answer, RESOLUTION_SHAPE);
}

static const runtime_identity_definition definition = {
    sizeof(runtime_identity_definition),
    NULL,
    parse,
    folder,
    resolve,
    release_parts,
    release_folder_answer,
    release_resolution,
    spisor_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_identity_definition_of(void) {
    return &definition;
}
