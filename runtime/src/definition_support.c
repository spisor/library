#include "definition_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.h"

int runtime_has_shape(uint32_t size, size_t shape) {
    return size >= shape;
}

spisor_status runtime_refuse_as(spisor_refusal *refusal, spisor_status status, const char *detail, const char *fix) {
    return spisor_refuse(refusal, detail, fix) == SPISOR_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY : status;
}

spisor_status runtime_refuse_json(spisor_refusal *refusal, cJSON *json_refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(json_refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(json_refusal, "fix");
    spisor_status status = SPISOR_OUT_OF_MEMORY;
    if (cJSON_IsString(message) && cJSON_IsString(fix)) {
        status = runtime_refuse_as(refusal, SPISOR_REFUSED, message->valuestring, fix->valuestring);
    }
    cJSON_Delete(json_refusal);
    return status;
}

spisor_status runtime_refuse_shape(spisor_refusal *refusal, const char *operation) {
    char detail[160];
    snprintf(detail, sizeof detail, "the argument or the answer of %s is absent or short of its first shape", operation);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail,
                             "pass the argument and an empty answer, each with its size set as SPISOR_INIT sets it");
}

void runtime_release_refusal(void *context, spisor_refusal *refusal) {
    (void)context;
    spisor_refusal_release(refusal);
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

spisor_status runtime_environment_json(const runtime_variable *environment, size_t count, cJSON **json) {
    *json = NULL;
    if (count > 0 && environment == NULL) {
        return SPISOR_MALFORMED;
    }
    for (size_t index = 0; index < count; index++) {
        if (environment[index].name == NULL || environment[index].value == NULL) {
            return SPISOR_MALFORMED;
        }
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
    runtime_variable *variables = malloc(((size_t)cJSON_GetArraySize(environment) + 1) * sizeof *variables);
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
    snprintf(fix, sizeof fix, "send %s as places' resolve answers it, an absolute directory", place);
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

spisor_status runtime_refuse_environment(spisor_refusal *refusal) {
    return runtime_refuse_as(refusal, SPISOR_MALFORMED, "an environment variable has no name or no value",
                             "send every variable with its name and its value, and environment_count no larger than "
                             "the variables sent");
}

void runtime_release_packed_answer(void *answer, size_t shape) {
    uint32_t size;
    void *storage;
    if (answer == NULL) {
        return;
    }
    memcpy(&size, answer, sizeof size);
    if (!runtime_has_shape(size, shape)) {
        return;
    }
    memcpy(&storage, (char *)answer + shape - sizeof storage, sizeof storage);
    runtime_release_storage(storage);
    memset((char *)answer + sizeof size, 0, shape - sizeof size);
}

void runtime_overwrite(void *bytes, size_t length) {
    volatile unsigned char *cursor = bytes;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

void runtime_overwrite_texts(const spisor_value *value) {
    size_t index;
    if (value->kind == SPISOR_VALUE_TEXT) {
        runtime_overwrite((char *)value->text, value->length);
    }
    for (index = 0; value->kind == SPISOR_VALUE_LIST && index < value->count; index++) {
        runtime_overwrite_texts(&value->items[index]);
    }
    for (index = 0; value->kind == SPISOR_VALUE_RECORD && index < value->count; index++) {
        runtime_overwrite((char *)value->entries[index].key, value->entries[index].key_length);
        runtime_overwrite_texts(&value->entries[index].value);
    }
}

void runtime_overwrite_json_texts(const cJSON *json) {
    const cJSON *child;
    if (cJSON_IsString(json)) {
        runtime_overwrite(json->valuestring, strlen(json->valuestring));
    }
    cJSON_ArrayForEach(child, json) {
        if (child->string != NULL) {
            runtime_overwrite(child->string, strlen(child->string));
        }
        runtime_overwrite_json_texts(child);
    }
}

void runtime_delete_secret_json(cJSON *json) {
    runtime_overwrite_json_texts(json);
    cJSON_Delete(json);
}

void runtime_release_storage(void *storage) {
    spisor_memory_release(storage);
}
