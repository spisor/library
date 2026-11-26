#include "hosts.h"
#include "allocation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_time.h"
#include "file_times.h"
#include "json_service.h"
#include "on_path.h"
#include "paths.h"
#include "platform_key.h"
#include "cJSON.h"
#include "spisor/path.h"

#define PROVENANCE_FIX                                                                                           \
    "pass the origin {\"method\", \"package\"?, \"options\"?, \"version\"?}, the application's binary, the home " \
    "as an absolute directory, the environment {<name>: <value>} and, when given, now as milliseconds since the " \
    "epoch"

static int copy_member(cJSON *target, const cJSON *source, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(source, name);
    cJSON *copy = value == NULL ? NULL : cJSON_Duplicate(value, 1);
    if (value != NULL && (copy == NULL || !cJSON_AddItemToObject(target, name, copy))) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

/** This build's os and architecture, Node's process.platform and process.arch, split from its platform key. */
static int add_platform(cJSON *provenance) {
    const char *key = runtime_platform_key();
    const char *hyphen = strchr(key, '-');
    size_t length = hyphen == NULL ? 0 : (size_t)(hyphen - key);
    char os[32];
    if (length == 0 || length >= sizeof os) {
        return 0;
    }
    memcpy(os, key, length);
    os[length] = '\0';
    return cJSON_AddStringToObject(provenance, "os", os) != NULL &&
           cJSON_AddStringToObject(provenance, "architecture", hyphen + 1) != NULL;
}

static int add_location(cJSON *provenance, const runtime_hosts_provenance_request *request) {
    char *found = NULL;
    runtime_on_path_status searched =
        runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, request->binary, request->environment, &found);
    int added = searched != RUNTIME_ON_PATH_OUT_OF_MEMORY &&
                cJSON_AddStringToObject(provenance, "location", found == NULL ? request->home : found) != NULL;
    spisor_memory_release(found);
    return added;
}

static int add_installed_at(cJSON *provenance, double now) {
    char text[RUNTIME_CATALOG_TIME_SIZE];
    return runtime_catalog_format_time(now, text) && cJSON_AddStringToObject(provenance, "installedAt", text) != NULL;
}

static double now_of(const runtime_hosts_provenance_request *request) {
    if (!request->has_now) {
        return floor(runtime_now_milliseconds());
    }
    return isfinite(request->now) && request->now >= 0 ? request->now : NAN;
}

static int is_writable_time(double now) {
    char text[RUNTIME_CATALOG_TIME_SIZE];
    return !isnan(now) && runtime_catalog_format_time(now, text);
}

static int is_provenance_request(const runtime_hosts_provenance_request *request) {
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request->origin, "method");
    return cJSON_IsObject(request->origin) && cJSON_IsString(method) && method->valuestring[0] != '\0' &&
           request->binary != NULL && request->home != NULL &&
           spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, request->home) && cJSON_IsObject(request->environment) &&
           is_writable_time(now_of(request));
}

cJSON *runtime_hosts_provenance(const runtime_hosts_provenance_request *request, cJSON **refusal) {
    *refusal = NULL;
    if (!is_provenance_request(request)) {
        *refusal = runtime_refusal("the call carries no usable origin, application binary, home, environment or now",
                                   PROVENANCE_FIX);
        return NULL;
    }
    const cJSON *origin = request->origin;
    cJSON *provenance = cJSON_CreateObject();
    int complete = provenance != NULL && copy_member(provenance, origin, "method") &&
                   copy_member(provenance, origin, "package") && copy_member(provenance, origin, "options") &&
                   copy_member(provenance, origin, "version") && add_location(provenance, request) &&
                   add_platform(provenance) && add_installed_at(provenance, now_of(request));
    if (!complete) {
        cJSON_Delete(provenance);
        return NULL;
    }
    return provenance;
}
