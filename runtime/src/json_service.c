#include "json_service.h"


cJSON *runtime_refusal(const char *message, const char *fix) {
    cJSON *refusal = cJSON_CreateObject();
    if (refusal != NULL && (cJSON_AddStringToObject(refusal, "message", message) == NULL ||
                            cJSON_AddStringToObject(refusal, "fix", fix) == NULL)) {
        cJSON_Delete(refusal);
        return NULL;
    }
    return refusal;
}

cJSON *runtime_refusal_of(const spisor_refusal *refusal) {
    return refusal->detail == NULL ? NULL : runtime_refusal(refusal->detail, refusal->fix == NULL ? "" : refusal->fix);
}

int runtime_is_refusal(const cJSON *refusal) {
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refusal, "message")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(refusal, "fix"));
}

cJSON *runtime_call_operation(runtime_json_operation operation, cJSON *argument, cJSON **refusal) {
    cJSON *answer = NULL;
    *refusal = NULL;
    if (argument != NULL) {
        answer = operation(argument, refusal);
    }
    cJSON_Delete(argument);
    return answer;
}

int runtime_add_owned(cJSON *object, const char *name, cJSON *item) {
    if (item == NULL || !cJSON_AddItemToObject(object, name, item)) {
        cJSON_Delete(item);
        return 0;
    }
    return 1;
}

int runtime_add_reference(cJSON *object, const char *name, const cJSON *item) {
    cJSON *reference = item == NULL            ? NULL
                       : cJSON_IsArray(item) ? cJSON_CreateArrayReference(item->child)
                                             : cJSON_CreateObjectReference(item->child);
    if (reference == NULL || !cJSON_AddItemToObject(object, name, reference)) {
        cJSON_Delete(reference);
        return 0;
    }
    return 1;
}
