#include "install_warnings.h"

#include <string.h>

#include "json_service.h"

#define KIND_NAMED_DEPENDENCY "named-dependency"
#define KIND_UNBOUND_SERVICE "unbound-service"
#define DETAIL_NAMED_DEPENDENCY \
    "this plugin finds the dependency by its name, so a rename can break it and whoever takes that name can replace it"
#define DETAIL_UNBOUND_SERVICE \
    "this plugin takes the service from whichever plugin provides it, so a rename can break it and whoever takes that name can replace it"

static int adds_warning(cJSON *warnings, const char *kind, const char *subject, const char *detail) {
    cJSON *warning = cJSON_CreateObject();
    if (warning == NULL || cJSON_AddStringToObject(warning, "kind", kind) == NULL ||
        cJSON_AddStringToObject(warning, "subject", subject) == NULL ||
        cJSON_AddStringToObject(warning, "detail", detail) == NULL || !cJSON_AddItemToArray(warnings, warning)) {
        cJSON_Delete(warning);
        return 0;
    }
    return 1;
}

static const char *text_member(const cJSON *object, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static int adds_named_dependencies(cJSON *warnings, const cJSON *manifest) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(manifest, "dependencies")) {
        const char *name = text_member(entry, "name");
        if (name != NULL && text_member(entry, "id") == NULL &&
            !adds_warning(warnings, KIND_NAMED_DEPENDENCY, name, DETAIL_NAMED_DEPENDENCY)) {
            return 0;
        }
    }
    return 1;
}

/** The id a consumes entry names, whether it is written as the id alone or as {id, provider?}. */
static const char *consumed_id(const cJSON *entry) {
    return cJSON_IsString(entry) ? entry->valuestring : text_member(entry, "id");
}

static int names_provider(const cJSON *entry) {
    return cJSON_IsObject(entry) && cJSON_GetObjectItemCaseSensitive(entry, "provider") != NULL;
}

static int adds_unbound_services(cJSON *warnings, const cJSON *manifest) {
    const cJSON *services = cJSON_GetObjectItemCaseSensitive(manifest, "services");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(services, "consumes")) {
        const char *id = consumed_id(entry);
        if (id != NULL && strchr(id, ':') != NULL && !names_provider(entry) &&
            !adds_warning(warnings, KIND_UNBOUND_SERVICE, id, DETAIL_UNBOUND_SERVICE)) {
            return 0;
        }
    }
    return 1;
}

static cJSON *warnings_of(const cJSON *manifest) {
    cJSON *answer = cJSON_CreateObject();
    cJSON *warnings = answer == NULL ? NULL : cJSON_AddArrayToObject(answer, "warnings");
    if (warnings == NULL || !adds_named_dependencies(warnings, manifest) || !adds_unbound_services(warnings, manifest)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *warnings(const cJSON *argument, cJSON **refusal) {
    const cJSON *manifest = cJSON_GetObjectItemCaseSensitive(argument, "manifest");
    if (!cJSON_IsObject(manifest)) {
        *refusal = runtime_refusal("the argument carries no manifest object", "send {\"manifest\": <the plugin.json content>}");
        return NULL;
    }
    return warnings_of(manifest);
}

RUNTIME_EXPORT int32_t runtime_install_warnings(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, warnings);
}
