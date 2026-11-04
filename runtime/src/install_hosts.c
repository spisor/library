#include "install_hosts.h"

#include <stdlib.h>

#include "hosts.h"
#include "install_support.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/path.h"

/** A binary name no PATH holds, since the empty environment hosts.provenance is given here carries no PATH. */
#define NO_BINARY "catalog"

char *runtime_install_own_home(const char *state, const char *id) {
    const char *segments[3] = {state, "home", id};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

static cJSON *provenance_call(const char *location, const char *version) {
    cJSON *call = cJSON_Parse("{\"origin\": {\"method\": \"catalog\"}, \"application\": {\"binary\": \"" NO_BINARY
                              "\"}, \"environment\": {}}");
    if (call == NULL || cJSON_AddStringToObject(cJSON_GetObjectItemCaseSensitive(call, "origin"), "version", version) == NULL ||
        cJSON_AddStringToObject(call, "home", location) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

cJSON *runtime_install_catalog_provenance(const char *cache, const char *id, const char *version, cJSON **refusal) {
    const char *segments[4] = {cache, "bundles", id, version};
    char *location = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 4);
    cJSON *provenance = location == NULL ? NULL
                                         : runtime_call_or_refuse(runtime_hosts_provenance, provenance_call(location, version),
                                                                  refusal);
    free(location);
    return provenance;
}

static cJSON *own_identity(const char *state, const char *id, const char *version, const cJSON *start) {
    char *home = runtime_install_own_home(state, id);
    cJSON *identity = home == NULL ? NULL : cJSON_CreateObject();
    if (identity == NULL || cJSON_AddStringToObject(identity, "id", id) == NULL ||
        cJSON_AddStringToObject(identity, "version", version) == NULL || cJSON_AddStringToObject(identity, "home", home) == NULL ||
        cJSON_AddFalseToObject(identity, "requiresCommand") == NULL || cJSON_AddArrayToObject(identity, "commands") == NULL ||
        !runtime_install_copy_into(identity, "start", start)) {
        cJSON_Delete(identity);
        identity = NULL;
    }
    free(home);
    return identity;
}

int runtime_install_register_own_host(const cJSON *places, const char *id, const char *version, const cJSON *start,
                                      cJSON **refusal) {
    cJSON *provenance = runtime_install_catalog_provenance(runtime_install_text(places, "cache"), id, version, refusal);
    cJSON *identity = provenance == NULL ? NULL : own_identity(runtime_install_text(places, "state"), id, version, start);
    cJSON *call = identity == NULL ? NULL : runtime_install_places_call(places);
    int built = call != NULL && cJSON_AddItemToObject(call, "identity", identity);
    identity = built ? NULL : identity;
    built = built && cJSON_AddItemToObject(call, "provenance", provenance);
    provenance = built ? NULL : provenance;
    built = built && cJSON_AddObjectToObject(call, "environment") != NULL;
    cJSON_Delete(identity);
    cJSON_Delete(provenance);
    if (!built) {
        cJSON_Delete(call);
        return 0;
    }
    cJSON *registered = runtime_call_or_refuse(runtime_hosts_register, call, refusal);
    cJSON_Delete(registered);
    return registered != NULL;
}
