#include "runtime/changes.h"

#include "answer_storage.h"
#include "change_kinds.h"
#include "changes.h"
#include "definition_support.h"
#include "changes_definition.h"
#include "json_service.h"
#include "json_value.h"
#include "paths.h"

#define CHANGE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_change, shell)
#define APPLY_SHAPE RUNTIME_SHAPE_THROUGH(runtime_changes_apply_argument, change)
#define REVERSE_SHAPE RUNTIME_SHAPE_THROUGH(runtime_changes_reverse_argument, host)
#define APPLY_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_changes_apply_answer, sequence)
#define REVERSE_ANSWER_SHAPE RUNTIME_SHAPE_THROUGH(runtime_changes_reverse_answer, storage)

#define CHANGE_FIX                                                                                                \
    "send a change of a known kind with the fields that kind names: a file's path and content, a directory's " \
    "path, a json-key's file, key and value, a profile-line's file and line, or a path-entry's directory and home"

static const char *const KIND_NAMES[] = {NULL, "file", "directory", "json-key", "profile-line", "path-entry"};

static spisor_status refuse_host(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no host",
                             "send host as the id the host registered with");
}

static spisor_status refuse_change(spisor_refusal *refusal, const char *detail) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, CHANGE_FIX);
}

static int add_text(cJSON *object, const char *name, const char *text) {
    return text == NULL || cJSON_AddStringToObject(object, name, text) != NULL;
}

static int add_flag(cJSON *object, const char *name, uint32_t flag) {
    return flag == 0 || cJSON_AddTrueToObject(object, name) != NULL;
}

/** The key's segments as a JSON array, leaving no key out; MALFORMED for a count without segments or a NULL one. */
static spisor_status add_key(cJSON *object, const runtime_change *change) {
    cJSON *key;
    if (change->key_count == 0) {
        return SPISOR_OK;
    }
    if (change->key == NULL) {
        return SPISOR_MALFORMED;
    }
    key = cJSON_AddArrayToObject(object, "key");
    for (size_t index = 0; key != NULL && index < change->key_count; index++) {
        cJSON *segment;
        if (change->key[index] == NULL) {
            return SPISOR_MALFORMED;
        }
        segment = cJSON_CreateString(change->key[index]);
        if (segment == NULL || !cJSON_AddItemToArray(key, segment)) {
            cJSON_Delete(segment);
            return SPISOR_OUT_OF_MEMORY;
        }
    }
    return key == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

static spisor_status add_value(cJSON *object, const spisor_value *value) {
    cJSON *json = NULL;
    spisor_status status = value == NULL ? SPISOR_OK : spisor_json_of_value(value, &json);
    if (status == SPISOR_OK && json != NULL && !cJSON_AddItemToObject(object, "value", json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

static int add_texts(cJSON *json, const runtime_change *change) {
    return add_text(json, "kind", KIND_NAMES[change->kind]) && add_text(json, "path", change->path) &&
           add_text(json, "content", change->content) && add_flag(json, "executable", change->executable) &&
           add_text(json, "file", change->file) && add_flag(json, "create", change->create) &&
           add_text(json, "line", change->line) && add_text(json, "directory", change->directory) &&
           add_text(json, "home", change->home) && add_text(json, "source", change->source) &&
           add_text(json, "shell", change->shell);
}

/** The change as the JSON record the change kinds read; NULL unless the answer is OK. */
static spisor_status change_json(const runtime_change *change, cJSON **json) {
    spisor_status status;
    *json = NULL;
    if (change->kind < RUNTIME_CHANGE_KIND_FILE || change->kind > RUNTIME_CHANGE_KIND_PATH_ENTRY) {
        return SPISOR_MALFORMED;
    }
    *json = cJSON_CreateObject();
    status = *json != NULL && add_texts(*json, change) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    if (status == SPISOR_OK) {
        status = add_key(*json, change);
    }
    if (status == SPISOR_OK) {
        status = add_value(*json, change->value);
    }
    if (status != SPISOR_OK) {
        cJSON_Delete(*json);
        *json = NULL;
    }
    return status;
}

/** The change as JSON once runtime_change_defect accepts it, refusing it as MALFORMED otherwise. */
static spisor_status usable_change(const runtime_change *change, cJSON **json, spisor_refusal *refusal) {
    const char *defect;
    spisor_status status;
    if (change == NULL || !runtime_has_shape(change->size, CHANGE_SHAPE)) {
        return refuse_change(refusal, "the change is absent or short of its first shape");
    }
    status = change_json(change, json);
    if (status == SPISOR_MALFORMED) {
        return refuse_change(refusal, "the change's kind, key or value is unusable");
    }
    defect = status == SPISOR_OK ? runtime_change_defect(*json) : NULL;
    if (defect != NULL) {
        cJSON_Delete(*json);
        *json = NULL;
        return refuse_change(refusal, defect);
    }
    return status;
}

static spisor_status status_of_outcome(const runtime_changes_outcome *outcome, runtime_changes_apply_answer *answer,
                                       spisor_refusal *refusal) {
    switch (outcome->answer) {
    case RUNTIME_CHANGES_RECORDED:
    case RUNTIME_CHANGES_UNNEEDED:
        answer->recorded = outcome->answer == RUNTIME_CHANGES_RECORDED ? 1 : 0;
        answer->sequence = outcome->sequence > 0 ? (uint64_t)outcome->sequence : 0;
        return SPISOR_OK;
    case RUNTIME_CHANGES_REFUSED:
        return runtime_refuse_as(refusal, SPISOR_REFUSED, outcome->reason, "nothing was changed or recorded");
    case RUNTIME_CHANGES_FAILED:
        return runtime_refuse_as(refusal, SPISOR_REFUSED, outcome->reason,
                                 "nothing was recorded; fix the cause and apply the change again");
    default:
        return SPISOR_OUT_OF_MEMORY;
    }
}

static spisor_status apply(void *context, const runtime_changes_apply_argument *argument,
                           runtime_changes_apply_answer *answer, spisor_refusal *refusal) {
    runtime_changes_outcome outcome;
    cJSON *change = NULL;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, APPLY_SHAPE) || answer == NULL ||
        !runtime_has_shape(answer->size, APPLY_ANSWER_SHAPE)) {
        return runtime_refuse_shape(refusal, "changes' apply");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    if (argument->host == NULL) {
        return refuse_host(refusal);
    }
    status = usable_change(argument->change, &change, refusal);
    if (status != SPISOR_OK) {
        return status;
    }
    outcome = runtime_changes_apply(argument->state, argument->host, change);
    cJSON_Delete(change);
    return status_of_outcome(&outcome, answer, refusal);
}

spisor_status runtime_fill_drifted(void **storage, const cJSON *drifted, const runtime_change_drift **out,
                                  size_t *count) {
    runtime_change_drift *filled;
    const cJSON *entry;
    size_t index = 0;
    if (!runtime_storage_take_array(storage, (size_t)cJSON_GetArraySize(drifted), sizeof *filled, (void **)&filled)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    cJSON_ArrayForEach(entry, drifted) {
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(entry, "sequence");
        filled[index].sequence = cJSON_IsNumber(sequence) ? (uint64_t)sequence->valuedouble : 0;
        if (!runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind")),
                                  &filled[index].kind) ||
            !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "detail")),
                                  &filled[index].detail)) {
            return SPISOR_OUT_OF_MEMORY;
        }
        index++;
    }
    *out = filled;
    *count = index;
    return SPISOR_OK;
}

static spisor_status fill_reversed(const cJSON *reversed, runtime_changes_reverse_answer *answer) {
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(reversed, "reversed");
    spisor_status status = runtime_fill_drifted(&answer->storage, cJSON_GetObjectItemCaseSensitive(reversed, "drifted"),
                                                &answer->drifted, &answer->drifted_count);
    if (status != SPISOR_OK) {
        runtime_release_stored_answer(answer, REVERSE_ANSWER_SHAPE);
        return status;
    }
    answer->reversed = cJSON_IsNumber(count) ? (uint64_t)count->valuedouble : 0;
    return SPISOR_OK;
}

static spisor_status reverse(void *context, const runtime_changes_reverse_argument *argument,
                             runtime_changes_reverse_answer *answer, spisor_refusal *refusal) {
    cJSON *operation_refusal = NULL;
    cJSON *reversed;
    spisor_status status;
    (void)context;
    if (argument == NULL || !runtime_has_shape(argument->size, REVERSE_SHAPE) ||
        !RUNTIME_IS_EMPTY_COUNTED_ANSWER(answer, REVERSE_ANSWER_SHAPE, answer->drifted_count)) {
        return runtime_refuse_shape(refusal, "changes' reverse");
    }
    if (!runtime_is_usable_home(argument->state)) {
        return runtime_refuse_place(refusal, "state");
    }
    if (argument->host == NULL) {
        return refuse_host(refusal);
    }
    reversed = runtime_changes_reverse(argument->state, argument->host, &operation_refusal);
    status = reversed == NULL ? runtime_refuse_json(refusal, operation_refusal) : fill_reversed(reversed, answer);
    cJSON_Delete(reversed);
    return status;
}

static void release_reverse_answer(void *context, runtime_changes_reverse_answer *answer) {
    (void)context;
    runtime_release_stored_answer(answer, REVERSE_ANSWER_SHAPE);
}

static const runtime_changes_definition definition = {
    sizeof(runtime_changes_definition), NULL, apply, reverse, release_reverse_answer, runtime_release_refusal,
};

RUNTIME_DEFINITION_API const void *runtime_changes_definition_of(void) {
    return &definition;
}
