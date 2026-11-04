#include "install_support.h"

#include <string.h>

#include "paths.h"
#include "spisor/path.h"

const char *runtime_install_text(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

const char *runtime_install_place(const cJSON *argument, const char *name) {
    const char *place = runtime_install_text(cJSON_GetObjectItemCaseSensitive(argument, "places"), name);
    return place != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place) ? place : NULL;
}

int runtime_install_copy_into(cJSON *object, const char *name, const cJSON *value) {
    if (value == NULL) {
        return 1;
    }
    cJSON *copy = cJSON_Duplicate(value, 1);
    if (copy == NULL || !cJSON_AddItemToObject(object, name, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

cJSON *runtime_install_places_call(const cJSON *places) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !runtime_install_copy_into(call, "places", places)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

int runtime_install_add_text(cJSON *array, const char *text) {
    cJSON *string = cJSON_CreateString(text);
    if (string == NULL || !cJSON_AddItemToArray(array, string)) {
        cJSON_Delete(string);
        return 0;
    }
    return 1;
}

int runtime_install_holds(const cJSON *array, const char *text) {
    for (const cJSON *entry = array == NULL ? NULL : array->child; entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, text) == 0) {
            return 1;
        }
    }
    return 0;
}
