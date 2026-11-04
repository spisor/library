#include "hosts.h"

#include "hosts_request.h"
#include "json_service.h"
#include "registry.h"
#include "spisor/json_kind.h"

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

static cJSON *list_for(const cJSON *argument, cJSON **refusal) {
    return runtime_call_or_refuse(runtime_registry_list, runtime_hosts_places_request(argument), refusal);
}

RUNTIME_EXPORT int32_t runtime_hosts_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, list_for);
}
