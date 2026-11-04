#include "json_service.h"

#include "catalog_answer.h"
#include "service_call.h"
#include "spisor/json_kind.h"

cJSON *runtime_refusal(const char *message, const char *fix) {
    cJSON *refusal = cJSON_CreateObject();
    if (refusal != NULL && (cJSON_AddStringToObject(refusal, "message", message) == NULL ||
                            cJSON_AddStringToObject(refusal, "fix", fix) == NULL)) {
        cJSON_Delete(refusal);
        return NULL;
    }
    return refusal;
}

int runtime_is_refusal(const cJSON *refusal) {
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refusal, "message")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refusal, "fix"));
}

static int32_t answer_or_refuse(cJSON *answer, cJSON *refusal, uint8_t **out, size_t *out_len) {
    if (answer != NULL || !runtime_is_refusal(refusal)) {
        cJSON_Delete(refusal);
        return runtime_catalog_answer(answer, out, out_len);
    }
    int32_t status = spisor_fail(cJSON_GetObjectItemCaseSensitive(refusal, "message")->valuestring,
                                 cJSON_GetObjectItemCaseSensitive(refusal, "fix")->valuestring, out, out_len);
    cJSON_Delete(refusal);
    return status;
}

int32_t runtime_serve_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len,
                           runtime_json_operation operation) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    cJSON *refusal = NULL;
    cJSON *answer = operation(call.argument, &refusal);
    status = answer_or_refuse(answer, refusal, out, out_len);
    spisor_discard_call(&call);
    return status;
}

cJSON *runtime_call_or_refuse(int32_t (*function)(const uint8_t *, size_t, uint8_t **, size_t *), cJSON *argument,
                              cJSON **refusal) {
    int32_t status = SPISOR_STATUS_OUT_OF_MEMORY;
    cJSON *answer = argument == NULL ? NULL : runtime_call_service(function, argument, &status);
    cJSON_Delete(argument);
    if (status == SPISOR_STATUS_SUCCESS) {
        return answer;
    }
    *refusal = status == SPISOR_STATUS_FAILURE ? answer : NULL;
    if (*refusal == NULL) {
        cJSON_Delete(answer);
    }
    return NULL;
}
