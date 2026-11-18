#include "install_hosts.h"
#include "allocation.h"

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

/** The catalog origin of version; NULL when memory runs out. */
static cJSON *catalog_origin(const char *version) {
    cJSON *origin = cJSON_CreateObject();
    if (origin == NULL || cJSON_AddStringToObject(origin, "method", "catalog") == NULL ||
        cJSON_AddStringToObject(origin, "version", version) == NULL) {
        cJSON_Delete(origin);
        return NULL;
    }
    return origin;
}

cJSON *runtime_install_catalog_provenance(const char *cache, const char *id, const char *version, cJSON **refusal) {
    const char *segments[4] = {cache, "bundles", id, version};
    char *location = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 4);
    cJSON *origin = location == NULL ? NULL : catalog_origin(version);
    cJSON *environment = origin == NULL ? NULL : cJSON_CreateObject();
    runtime_hosts_provenance_request request = {origin, NO_BINARY, location, environment, 0, 0};
    cJSON *provenance = environment == NULL ? NULL : runtime_hosts_provenance(&request, refusal);
    if (environment == NULL) {
        *refusal = NULL;
    }
    cJSON_Delete(origin);
    cJSON_Delete(environment);
    spisor_memory_release(location);
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
    spisor_memory_release(home);
    return identity;
}

int runtime_install_register_own_host(const cJSON *places, const char *id, const char *version, const cJSON *start,
                                      cJSON **refusal) {
    cJSON *provenance = runtime_install_catalog_provenance(runtime_install_text(places, "cache"), id, version, refusal);
    cJSON *identity = provenance == NULL ? NULL : own_identity(runtime_install_text(places, "state"), id, version, start);
    cJSON *environment = identity == NULL ? NULL : cJSON_CreateObject();
    runtime_hosts_registration_request request = {runtime_install_text(places, "state"), identity, provenance,
                                                  environment};
    cJSON *registered = environment == NULL ? NULL : runtime_hosts_register(&request, refusal);
    cJSON_Delete(environment);
    cJSON_Delete(identity);
    cJSON_Delete(provenance);
    cJSON_Delete(registered);
    return registered != NULL;
}
