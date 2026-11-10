#include "identity.h"

#include <stdlib.h>
#include <string.h>

#include "spisor/json_kind.h"

static int attach_part(cJSON *answer, const char *name, runtime_identity_part part) {
    if (part.start == NULL) {
        return 1;
    }
    char *text = malloc(part.length + 1);
    if (text == NULL) {
        return 0;
    }
    memcpy(text, part.start, part.length);
    text[part.length] = '\0';
    int attached = cJSON_AddStringToObject(answer, name, text) != NULL;
    free(text);
    return attached;
}

static cJSON *parts_answer(const runtime_identity *parsed) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddTrueToObject(answer, "valid") == NULL || !attach_part(answer, "resolver", parsed->resolver) ||
        !attach_part(answer, "host", parsed->host) || !attach_part(answer, "number", parsed->number) ||
        !attach_part(answer, "path", parsed->path)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *invalid_answer(void) {
    cJSON *answer = cJSON_CreateObject();
    if (answer != NULL && cJSON_AddFalseToObject(answer, "valid") == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static int32_t answer(cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        *out = NULL;
        *out_len = 0;
        return SPISOR_STATUS_OUT_OF_MEMORY;
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int32_t parse(const char *id, uint8_t **out, size_t *out_len) {
    runtime_identity parsed;
    return answer(runtime_identity_parse_id(id, &parsed) ? parts_answer(&parsed) : invalid_answer(), out, out_len);
}

static cJSON *folder_answer(const char *folder) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && cJSON_AddStringToObject(result, "folder", folder) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return result;
}

static int32_t refuse_folder(runtime_identity_folder_status status, uint8_t **out, size_t *out_len) {
    if (status == RUNTIME_IDENTITY_FOLDER_INVALID) {
        return spisor_fail("the id is not valid", "send an id such as github:<number>[/<path>] or local:<name>", out, out_len);
    }
    if (status == RUNTIME_IDENTITY_FOLDER_AMBIGUOUS) {
        return spisor_fail("the local id would share a folder with another id",
                           "use the plugin's import name: lowercase, without +, = or :", out, out_len);
    }
    return spisor_fail("the id cannot name a folder", "use a local name without path separators, wildcards or a trailing dot", out, out_len);
}

static int32_t folder(const char *id, uint8_t **out, size_t *out_len) {
    char *name = NULL;
    runtime_identity_folder_status status = runtime_identity_folder_name(id, &name);
    if (status == RUNTIME_IDENTITY_FOLDER_OUT_OF_MEMORY) {
        return answer(NULL, out, out_len);
    }
    if (status != RUNTIME_IDENTITY_FOLDER_NAMED) {
        return refuse_folder(status, out, out_len);
    }
    int32_t result = answer(folder_answer(name), out, out_len);
    free(name);
    return result;
}

static int32_t serve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                     int32_t (*operation)(const char *id, uint8_t **out, size_t *out_len)) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(call.argument, "id");
    status = cJSON_IsString(id) ? operation(id->valuestring, out, out_len)
                                : spisor_fail("the argument carries no id", "send {\"id\": \"<id>\"}", out, out_len);
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_identity_parse(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, parse);
}

RUNTIME_EXPORT int32_t runtime_identity_folder(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, out, out_len, folder);
}
