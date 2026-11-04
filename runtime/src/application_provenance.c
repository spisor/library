#include "hosts.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_time.h"
#include "file_times.h"
#include "json_service.h"
#include "on_path.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define PROVENANCE_FIX                                                                                         \
    "send {\"origin\": {\"method\", \"package\"?, \"options\"?, \"version\"?}, \"application\": {\"binary\"}, " \
    "\"home\": \"<absolute directory>\", \"environment\": {<name>: <value>}, \"now\"?: <milliseconds since the epoch>}"

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

static int add_location(cJSON *provenance, const cJSON *argument) {
    const char *binary = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "application"), "binary"));
    char *found = runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary,
                                       cJSON_GetObjectItemCaseSensitive(argument, "environment"));
    const char *home = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "home"));
    int added = cJSON_AddStringToObject(provenance, "location", found == NULL ? home : found) != NULL;
    free(found);
    return added;
}

static int add_installed_at(cJSON *provenance, double now) {
    char text[RUNTIME_CATALOG_TIME_SIZE];
    return runtime_catalog_format_time(now, text) && cJSON_AddStringToObject(provenance, "installedAt", text) != NULL;
}

static double now_of(const cJSON *argument) {
    const cJSON *now = cJSON_GetObjectItemCaseSensitive(argument, "now");
    if (now == NULL) {
        return floor(runtime_now_milliseconds());
    }
    return cJSON_IsNumber(now) && isfinite(now->valuedouble) && now->valuedouble >= 0 ? now->valuedouble : NAN;
}

static int is_writable_time(double now) {
    char text[RUNTIME_CATALOG_TIME_SIZE];
    return !isnan(now) && runtime_catalog_format_time(now, text);
}

static int is_provenance_argument(const cJSON *argument) {
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(argument, "origin");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(origin, "method");
    const cJSON *binary =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "application"), "binary");
    const char *home = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(argument, "home"));
    return cJSON_IsObject(origin) && cJSON_IsString(method) && method->valuestring[0] != '\0' && cJSON_IsString(binary) &&
           home != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, home) &&
           cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(argument, "environment")) && is_writable_time(now_of(argument));
}

static cJSON *provenance_for(const cJSON *argument, cJSON **refusal) {
    if (!is_provenance_argument(argument)) {
        *refusal = runtime_refusal("the argument carries no usable origin, application, home, environment or now",
                                   PROVENANCE_FIX);
        return NULL;
    }
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(argument, "origin");
    cJSON *provenance = cJSON_CreateObject();
    int complete = provenance != NULL && copy_member(provenance, origin, "method") &&
                   copy_member(provenance, origin, "package") && copy_member(provenance, origin, "options") &&
                   copy_member(provenance, origin, "version") && add_location(provenance, argument) &&
                   add_platform(provenance) && add_installed_at(provenance, now_of(argument));
    if (!complete) {
        cJSON_Delete(provenance);
        return NULL;
    }
    return provenance;
}

RUNTIME_EXPORT int32_t runtime_hosts_provenance(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, provenance_for);
}
