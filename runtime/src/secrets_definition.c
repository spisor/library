#include "runtime/secrets.h"

#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "secrets.h"

#define ENTRY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_entry_argument, name)
#define SET_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_set_argument, value)
#define CLEAR_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_clear_argument, plugin)
#define RECONCILE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_reconcile_argument, places)
#define GET_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_get_answer, storage)
#define SET_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_set_answer, location)
#define RECONCILE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_secrets_reconcile_answer, moved)

/** The place `places` names; 0 when it names no absolute secrets file. */
static int place_of(const runtime_secret_places *places, runtime_secret_place *place) {
    if (places == NULL) {
        return 0;
    }
    place->secrets_file = places->secrets_file;
    place->force_file_store = places->force_file_store != 0;
    return runtime_is_usable_secret_place(place);
}

/** Overwrites a secret's bytes, through a volatile pointer so the compiler cannot drop a write to memory it frees. */
static void overwrite(char *bytes, size_t length) {
    volatile char *cursor = bytes;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

static spisor_status fill_secret(char *value, runtime_secrets_get_answer *answer) {
    const char *packed;
    size_t length = strlen(value);
    int filled = runtime_pack_texts((const char *const *)&value, &packed, 1, &answer->storage);
    overwrite(value, length);
    free(value);
    if (!filled) {
        return SPISOR_OUT_OF_MEMORY;
    }
    answer->value = packed;
    answer->length = length;
    return SPISOR_OK;
}

static spisor_status get(void *context, const runtime_secrets_entry_argument *argument,
                         runtime_secrets_get_answer *answer, spisor_refusal *refusal) {
    runtime_secret_place place;
    char *value = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ENTRY_SHAPE) ||
        !RUNTIME_IS_EMPTY_ANSWER(answer, GET_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "secrets' get");
    }
    if (!place_of(argument->places, &place)) {
        return runtime_refuse_secret_place(refusal);
    }
    status = runtime_get_secret(&place, argument->plugin, argument->name, &value, refusal);
    return status == SPISOR_OK ? fill_secret(value, answer) : status;
}

static spisor_status set(void *context, const runtime_secrets_set_argument *argument,
                         runtime_secrets_set_answer *answer, spisor_refusal *refusal) {
    runtime_secret_place place;
    runtime_secret_location location = RUNTIME_SECRET_LOCATION_FILE;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, SET_SHAPE) || answer == NULL ||
        !runtime_has_shape(answer->size, SET_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "secrets' set");
    }
    if (!place_of(argument->places, &place)) {
        return runtime_refuse_secret_place(refusal);
    }
    status = runtime_set_secret(&place, argument->plugin, argument->name, argument->value, &location, refusal);
    if (status == SPISOR_OK) {
        answer->location = (uint32_t)location;
    }
    return status;
}

static spisor_status remove_entry(void *context, const runtime_secrets_entry_argument *argument,
                                  spisor_refusal *refusal) {
    runtime_secret_place place;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, ENTRY_SHAPE)) {
        return runtime_refuse_shape(refusal, "secrets' remove");
    }
    if (!place_of(argument->places, &place)) {
        return runtime_refuse_secret_place(refusal);
    }
    return runtime_delete_secret(&place, argument->plugin, argument->name, refusal);
}

static spisor_status clear(void *context, const runtime_secrets_clear_argument *argument, spisor_refusal *refusal) {
    runtime_secret_place place;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, CLEAR_SHAPE)) {
        return runtime_refuse_shape(refusal, "secrets' clear");
    }
    if (!place_of(argument->places, &place)) {
        return runtime_refuse_secret_place(refusal);
    }
    return runtime_clear_secrets(&place, argument->plugin, refusal);
}

static spisor_status reconcile(void *context, const runtime_secrets_reconcile_argument *argument,
                               runtime_secrets_reconcile_answer *answer, spisor_refusal *refusal) {
    runtime_secret_place place;
    uint64_t moved = 0;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, RECONCILE_SHAPE) || answer == NULL ||
        !runtime_has_shape(answer->size, RECONCILE_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "secrets' reconcile");
    }
    if (!place_of(argument->places, &place)) {
        return runtime_refuse_secret_place(refusal);
    }
    status = runtime_reconcile_secrets(&place, &moved, refusal);
    if (status == SPISOR_OK) {
        answer->moved = moved;
    }
    return status;
}

static void release_get_answer(void *context, runtime_secrets_get_answer *answer) {
    (void)context;
    if (answer == NULL || !runtime_has_shape(answer->size, GET_ANSWER_SHAPE)) {
        return;
    }
    if (answer->storage != NULL) {
        overwrite(answer->storage, answer->length);
    }
    runtime_release_storage(answer->storage);
    RUNTIME_CLEAR_ANSWER(answer, GET_ANSWER_SHAPE);
}

static const runtime_secrets_definition definition = {
    sizeof(runtime_secrets_definition), NULL, get, set, remove_entry, clear, reconcile, release_get_answer,
    runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_secrets_definition_of(void) {
    return &definition;
}
