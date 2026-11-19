#include "definition_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "answer_storage.h"
#include "spisor/json_value.h"
#include "paths.h"
#include "spisor/path.h"

spisor_status runtime_serve_answered(cJSON *answered, cJSON *json_refusal, runtime_answer_filler fill, void *answer,
                                     size_t answer_shape, spisor_refusal *refusal) {
    spisor_status status = answered == NULL ? runtime_refuse_json(refusal, json_refusal) : fill(answered, answer);
    if (answered != NULL && status != SPISOR_OK) {
        runtime_release_stored_answer(answer, answer_shape);
    }
    cJSON_Delete(answered);
    return status;
}

static spisor_status value_json(const spisor_value *value, spisor_value_kind kind, cJSON **json) {
    spisor_status status = value == NULL ? SPISOR_MALFORMED : spisor_value_check(value);
    *json = NULL;
    if (status == SPISOR_OK && value->kind != (uint32_t)kind) {
        status = SPISOR_MALFORMED;
    }
    return status == SPISOR_OK ? spisor_json_of_value(value, json) : status;
}

spisor_status runtime_add_value_json(cJSON *call, const char *name, const spisor_value *value, spisor_value_kind kind) {
    cJSON *json = NULL;
    spisor_status status = value_json(value, kind, &json);
    if (status == SPISOR_OK && !cJSON_AddItemToObject(call, name, json)) {
        cJSON_Delete(json);
        status = SPISOR_OUT_OF_MEMORY;
    }
    return status;
}

spisor_status runtime_refuse_json(spisor_refusal *refusal, cJSON *json_refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(json_refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(json_refusal, "fix");
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    if (cJSON_IsString(message) && cJSON_IsString(fix)) {
        status = spisor_refuse_as(refusal, SPISOR_REFUSED, message->valuestring, fix->valuestring);
    }
    cJSON_Delete(json_refusal);
    return status;
}

int runtime_pack_texts(const char *const *texts, const char **packed, size_t count, void **storage) {
    size_t bytes = 1;
    char *cursor;
    for (size_t index = 0; index < count; index++) {
        bytes += texts[index] == NULL ? 0 : strlen(texts[index]) + 1;
    }
    *storage = spisor_memory_allocate(bytes);
    if (*storage == NULL) {
        return 0;
    }
    cursor = *storage;
    for (size_t index = 0; index < count; index++) {
        size_t length = texts[index] == NULL ? 0 : strlen(texts[index]) + 1;
        packed[index] = texts[index] == NULL ? NULL : memcpy(cursor, texts[index], length);
        cursor += length;
    }
    return 1;
}

int runtime_is_text(const char *text) {
    return text != NULL && text[0] != '\0';
}

int runtime_is_place(const char *place) {
    return runtime_is_text(place) && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place);
}

int runtime_are_arguments(const char *const *texts, size_t count) {
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

int runtime_are_texts(const char *const *texts, size_t count, int unique) {
    if (!runtime_are_arguments(texts, count)) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!runtime_is_text(texts[index])) {
            return 0;
        }
        for (size_t earlier = 0; unique && earlier < index; earlier++) {
            if (strcmp(texts[earlier], texts[index]) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

int runtime_are_item_versions(const runtime_item_version *versions, size_t count) {
    if (count > 0 && versions == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!runtime_is_text(versions[index].id) || !runtime_is_text(versions[index].version)) {
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

static int is_variable_name(const char *name) {
    return runtime_is_text(name) && strchr(name, '=') == NULL;
}

int runtime_is_environment(const runtime_variable *environment, size_t count) {
    if (count > 0 && environment == NULL) {
        return 0;
    }
    for (size_t index = 0; index < count; index++) {
        if (!is_variable_name(environment[index].name) || environment[index].value == NULL) {
            return 0;
        }
        for (size_t earlier = 0; earlier < index; earlier++) {
            if (strcmp(environment[earlier].name, environment[index].name) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

spisor_status runtime_start_storage(void **storage) {
    return runtime_storage_take(storage, 1) == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

spisor_status runtime_refuse_member(spisor_refusal *refusal, const char *member, const char *header) {
    char detail[160];
    char fix[200];
    snprintf(detail, sizeof detail, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "pass %s as %s describes it", member, header);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

spisor_status runtime_environment_json(const runtime_variable *environment, size_t count, cJSON **json) {
    *json = NULL;
    if (!runtime_is_environment(environment, count)) {
        return SPISOR_MALFORMED;
    }
    *json = cJSON_CreateObject();
    for (size_t index = 0; *json != NULL && index < count; index++) {
        if (cJSON_AddStringToObject(*json, environment[index].name, environment[index].value) == NULL) {
            cJSON_Delete(*json);
            *json = NULL;
        }
    }
    return *json == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

runtime_variable *runtime_variables_of_json(const cJSON *environment, size_t *count) {
    const cJSON *member;
    size_t variable_count = (size_t)cJSON_GetArraySize(environment) + 1;
    runtime_variable *variables = spisor_memory_allocate(variable_count * sizeof *variables);
    *count = 0;
    cJSON_ArrayForEach(member, environment) {
        if (variables != NULL && cJSON_IsString(member) && member->string != NULL) {
            variables[*count].name = member->string;
            variables[*count].value = member->valuestring;
            (*count)++;
        }
    }
    return variables;
}

spisor_status runtime_refuse_place(spisor_refusal *refusal, const char *place) {
    char detail[128];
    char fix[192];
    snprintf(detail, sizeof detail, "the %s place is empty or not an absolute directory", place);
    snprintf(fix, sizeof fix, "pass %s as places' resolve answers it, an absolute directory", place);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

spisor_status runtime_refuse_environment(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED,
                             "an environment variable has no name or no value, a name holding =, or a name given twice",
                             "pass every variable once with its name and its value, and environment_count no larger "
                             "than the variables sent");
}

void runtime_release_packed_answer(void *answer, size_t shape) {
    uint32_t size;
    void *storage;
    if (answer == NULL) {
        return;
    }
    memcpy(&size, answer, sizeof size);
    if (!spisor_has_shape(size, shape)) {
        return;
    }
    memcpy(&storage, (char *)answer + shape - sizeof storage, sizeof storage);
    runtime_release_storage(storage);
    memset((char *)answer + sizeof size, 0, shape - sizeof size);
}

void runtime_overwrite_texts(const spisor_value *value) {
    size_t index;
    if (value->kind == SPISOR_VALUE_TEXT) {
        spisor_overwrite((char *)value->text, value->length);
    }
    for (index = 0; value->kind == SPISOR_VALUE_LIST && index < value->count; index++) {
        runtime_overwrite_texts(&value->items[index]);
    }
    for (index = 0; value->kind == SPISOR_VALUE_RECORD && index < value->count; index++) {
        spisor_overwrite((char *)value->entries[index].key, value->entries[index].key_length);
        runtime_overwrite_texts(&value->entries[index].value);
    }
}

void runtime_overwrite_json_texts(const cJSON *json) {
    const cJSON *child;
    if (cJSON_IsString(json)) {
        spisor_overwrite(json->valuestring, strlen(json->valuestring));
    }
    cJSON_ArrayForEach(child, json) {
        if (child->string != NULL) {
            spisor_overwrite(child->string, strlen(child->string));
        }
        runtime_overwrite_json_texts(child);
    }
}

void runtime_release_storage(void *storage) {
    spisor_memory_release(storage);
}
