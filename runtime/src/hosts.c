#include "hosts_request.h"

cJSON *runtime_hosts_places_request(const cJSON *argument) {
    cJSON *request = cJSON_CreateObject();
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    cJSON *copy = places == NULL ? NULL : cJSON_Duplicate(places, 1);
    if (request == NULL || (places != NULL && (copy == NULL || !cJSON_AddItemToObject(request, "places", copy)))) {
        cJSON_Delete(request);
        cJSON_Delete(copy);
        return NULL;
    }
    return request;
}
