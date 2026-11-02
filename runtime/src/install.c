#include "install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "spisor/json_file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

#define MARKETPLACE_METHOD "marketplace"
#define PACKAGES_FILE_NAME "packages.json"
#define MESSAGE_SIZE 1200
#define FIX_SIZE 2400

typedef enum install_status { INSTALL_OK, INSTALL_REFUSED, INSTALL_OUT_OF_MEMORY } install_status;

typedef struct refusal {
    char message[MESSAGE_SIZE];
    char fix[FIX_SIZE];
} refusal;

static install_status refuse(refusal *reason, const char *message, const char *fix) {
    snprintf(reason->message, sizeof reason->message, "%s", message);
    snprintf(reason->fix, sizeof reason->fix, "%s", fix);
    return INSTALL_REFUSED;
}

static const cJSON *member(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int is_text(const cJSON *value) {
    return cJSON_IsString(value) && value->valuestring[0] != '\0';
}

static int is_mapping(const cJSON *value) {
    if (is_text(value)) {
        return 1;
    }
    const cJSON *options = member(value, "options");
    return cJSON_IsObject(value) && is_text(member(value, "name")) && (options == NULL || cJSON_IsObject(options));
}

static cJSON *normalized_mapping(const cJSON *value) {
    const cJSON *name = cJSON_IsObject(value) ? member(value, "name") : value;
    const cJSON *options = cJSON_IsObject(value) ? member(value, "options") : NULL;
    cJSON *mapping = cJSON_CreateObject();
    int built = mapping != NULL && cJSON_AddStringToObject(mapping, "name", name->valuestring) != NULL;
    if (built && options != NULL) {
        cJSON *copy = cJSON_Duplicate(options, 1);
        built = copy != NULL && cJSON_AddItemToObject(mapping, "options", copy);
        if (!built) {
            cJSON_Delete(copy);
        }
    }
    if (!built) {
        cJSON_Delete(mapping);
        return NULL;
    }
    return mapping;
}

static install_status put_mapping(cJSON *merged, const char *method, const cJSON *value) {
    cJSON *mapping = normalized_mapping(value);
    if (mapping == NULL) {
        return INSTALL_OUT_OF_MEMORY;
    }
    int placed = member(merged, method) != NULL ? cJSON_ReplaceItemInObjectCaseSensitive(merged, method, mapping)
                                                : cJSON_AddItemToObject(merged, method, mapping);
    if (!placed) {
        cJSON_Delete(mapping);
        return INSTALL_OUT_OF_MEMORY;
    }
    return INSTALL_OK;
}

static install_status refuse_mapping(refusal *reason, const char *method, const char *origin) {
    snprintf(reason->message, sizeof reason->message,
             "the package mapping for method \"%s\" in %s is neither a package name nor { name, options }", method,
             origin);
    snprintf(reason->fix, sizeof reason->fix,
             "change the mapping for method \"%s\" in %s to a package name, or to an object with a \"name\" string "
             "and optionally an \"options\" object",
             method, origin);
    return INSTALL_REFUSED;
}

static install_status merge_mappings(cJSON *merged, const cJSON *source, const char *origin, refusal *reason) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, source) {
        if (!is_mapping(entry)) {
            return refuse_mapping(reason, entry->string, origin);
        }
        install_status status = put_mapping(merged, entry->string, entry);
        if (status != INSTALL_OK) {
            return status;
        }
    }
    return INSTALL_OK;
}

static install_status merge_manifest(cJSON *merged, const cJSON *manifest_packages, refusal *reason) {
    if (manifest_packages == NULL) {
        return INSTALL_OK;
    }
    if (!cJSON_IsObject(manifest_packages)) {
        return refuse(reason, "manifestPackages is not an object keyed by install method",
                      "send manifestPackages as { \"<method>\": \"<package name>\" | { \"name\", \"options\" } }");
    }
    return merge_mappings(merged, manifest_packages, "manifestPackages", reason);
}

static char *packages_file_path(const char *portable) {
    const char *segments[2] = {portable, PACKAGES_FILE_NAME};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static install_status refuse_file(refusal *reason, const char *path) {
    snprintf(reason->message, sizeof reason->message,
             "the user's package mappings file %s is unreadable, not JSON, or not shaped by item and method", path);
    snprintf(reason->fix, sizeof reason->fix,
             "repair %s so it holds { \"<item id>\": { \"<method>\": <mapping> } }, or remove it", path);
    return INSTALL_REFUSED;
}

static install_status merge_item_entry(cJSON *merged, const cJSON *root, const char *path, const char *item,
                                       refusal *reason) {
    const cJSON *mappings = member(root, item);
    if (mappings == NULL) {
        return INSTALL_OK;
    }
    return cJSON_IsObject(mappings) ? merge_mappings(merged, mappings, path, reason) : refuse_file(reason, path);
}

static install_status merge_user_file(cJSON *merged, const char *path, const char *item, refusal *reason) {
    spisor_json_file_state state;
    cJSON *root = spisor_read_json_file(path, &state);
    if (state == SPISOR_JSON_FILE_ABSENT) {
        return INSTALL_OK;
    }
    install_status status = state == SPISOR_JSON_FILE_PARSED && cJSON_IsObject(root)
                                ? merge_item_entry(merged, root, path, item, reason)
                                : refuse_file(reason, path);
    cJSON_Delete(root);
    return status;
}

static install_status merge_user(cJSON *merged, const char *portable, const char *item, refusal *reason) {
    char *path = packages_file_path(portable);
    if (path == NULL) {
        return INSTALL_OUT_OF_MEMORY;
    }
    install_status status = merge_user_file(merged, path, item, reason);
    free(path);
    return status;
}

typedef struct identity {
    const char *portable;
    const char *item;
} identity;

static install_status read_identity(const cJSON *argument, identity *who, refusal *reason) {
    const cJSON *portable = member(member(argument, "places"), "portable");
    const cJSON *item = member(argument, "item");
    if (!cJSON_IsString(portable) || !runtime_is_usable_home(portable->valuestring) || !is_text(item)) {
        return refuse(reason, "the argument carries no absolute places.portable and no item id",
                      "send places as places.resolve answers it, and item as the marketplace item's id");
    }
    who->portable = portable->valuestring;
    who->item = item->valuestring;
    return INSTALL_OK;
}

static install_status merge_both(cJSON *merged, const cJSON *argument, const identity *who, refusal *reason) {
    install_status status = merge_manifest(merged, member(argument, "manifestPackages"), reason);
    return status == INSTALL_OK ? merge_user(merged, who->portable, who->item, reason) : status;
}

static install_status packages_of(const cJSON *argument, cJSON **merged, refusal *reason) {
    identity who;
    install_status status = read_identity(argument, &who, reason);
    if (status != INSTALL_OK) {
        return status;
    }
    *merged = cJSON_CreateObject();
    status = *merged == NULL ? INSTALL_OUT_OF_MEMORY : merge_both(*merged, argument, &who, reason);
    if (status != INSTALL_OK) {
        cJSON_Delete(*merged);
        *merged = NULL;
    }
    return status;
}

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t answer_member(const char *name, cJSON *value, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL || !cJSON_AddItemToObject(result, name, value)) {
        cJSON_Delete(value);
        cJSON_Delete(result);
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int32_t answer_failure(install_status status, const refusal *reason, uint8_t **out, size_t *out_len) {
    if (status == INSTALL_REFUSED) {
        return spisor_fail(reason->message, reason->fix, out, out_len);
    }
    return out_of_memory(out, out_len);
}

RUNTIME_EXPORT int32_t runtime_install_packages(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t parsed = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (parsed != SPISOR_STATUS_SUCCESS) {
        return parsed;
    }
    refusal reason;
    cJSON *merged = NULL;
    install_status status = packages_of(call.argument, &merged, &reason);
    int32_t answered = status == INSTALL_OK ? answer_member("packages", merged, out, out_len)
                                            : answer_failure(status, &reason, out, out_len);
    spisor_discard_call(&call);
    return answered;
}

static int lists_text(const cJSON *list, const char *text) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, list) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, text) == 0) {
            return 1;
        }
    }
    return 0;
}

static const cJSON *first_provider(const cJSON *catalog, const char *method) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, catalog) {
        const cJSON *offered = member(entry, "installMethod");
        if (cJSON_IsString(offered) && strcmp(offered->valuestring, method) == 0) {
            return entry;
        }
    }
    return NULL;
}

static int is_text_array(const cJSON *list) {
    const cJSON *entry;
    if (!cJSON_IsArray(list)) {
        return 0;
    }
    cJSON_ArrayForEach(entry, list) {
        if (!is_text(entry)) {
            return 0;
        }
    }
    return 1;
}

typedef struct plan_request {
    const char *chosen;
    const char *item;
    const cJSON *methods;
    const cJSON *installed_providers;
    const cJSON *catalog;
} plan_request;

static int is_plan_argument(const cJSON *argument) {
    const cJSON *installed = member(argument, "installedProviders");
    const cJSON *catalog = member(argument, "catalog");
    return is_text(member(argument, "chosen")) && is_text_array(member(argument, "methods")) &&
           (installed == NULL || is_text_array(installed)) && (catalog == NULL || cJSON_IsArray(catalog));
}

static install_status read_plan_request(const cJSON *argument, plan_request *request, refusal *reason) {
    if (!is_plan_argument(argument)) {
        return refuse(reason, "the argument carries no chosen method, or methods and installedProviders that are not "
                              "arrays of method ids",
                      "send chosen as a method id, methods and installedProviders as arrays of method ids, and "
                      "catalog as an array of catalog entries");
    }
    const cJSON *item = member(argument, "item");
    request->chosen = member(argument, "chosen")->valuestring;
    request->item = is_text(item) ? item->valuestring : "";
    request->methods = member(argument, "methods");
    request->installed_providers = member(argument, "installedProviders");
    request->catalog = member(argument, "catalog");
    return INSTALL_OK;
}

static install_status refuse_unlisted(refusal *reason, const plan_request *request) {
    snprintf(reason->message, sizeof reason->message, "the item \"%s\" does not list the install method \"%s\"",
             request->item, request->chosen);
    snprintf(reason->fix, sizeof reason->fix, "choose one of the methods the item lists; \"%s\" is not among them",
             request->chosen);
    return INSTALL_REFUSED;
}

static install_status refuse_without_package(refusal *reason, const plan_request *request) {
    snprintf(reason->message, sizeof reason->message, "the item \"%s\" has no package under the install method \"%s\"",
             request->item, request->chosen);
    snprintf(reason->fix, sizeof reason->fix,
             "add a \"%s\" mapping for \"%s\" to packages.json in the portable place, or choose another method",
             request->chosen, request->item);
    return INSTALL_REFUSED;
}

static install_status refuse_without_provider(refusal *reason, const plan_request *request) {
    snprintf(reason->message, sizeof reason->message,
             "no added marketplace offers a provider for the install method \"%s\"", request->chosen);
    snprintf(reason->fix, sizeof reason->fix,
             "add a marketplace that lists a provider for \"%s\", or choose another method", request->chosen);
    return INSTALL_REFUSED;
}

static cJSON *install_step(const char *method, const cJSON *mapping) {
    cJSON *step = cJSON_CreateObject();
    cJSON *detail = cJSON_CreateObject();
    cJSON *package = mapping == NULL ? NULL : cJSON_Duplicate(mapping, 1);
    int built = step != NULL && detail != NULL && (mapping == NULL || package != NULL) &&
                cJSON_AddStringToObject(detail, "method", method) != NULL &&
                (package == NULL || cJSON_AddItemToObject(detail, "package", package));
    if (built) {
        package = NULL;
        built = cJSON_AddItemToObject(step, "install", detail);
        detail = built ? NULL : detail;
    }
    cJSON_Delete(package);
    cJSON_Delete(detail);
    if (!built) {
        cJSON_Delete(step);
        return NULL;
    }
    return step;
}

static cJSON *provider_step(const cJSON *entry) {
    cJSON *step = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(entry, 1);
    if (step == NULL || copy == NULL || !cJSON_AddItemToObject(step, "install-provider", copy)) {
        cJSON_Delete(copy);
        cJSON_Delete(step);
        return NULL;
    }
    return step;
}

static install_status add_step(cJSON *steps, cJSON *step) {
    if (step == NULL || !cJSON_AddItemToArray(steps, step)) {
        cJSON_Delete(step);
        return INSTALL_OUT_OF_MEMORY;
    }
    return INSTALL_OK;
}

static install_status add_provider_step(cJSON *steps, const plan_request *request, refusal *reason) {
    if (lists_text(request->installed_providers, request->chosen)) {
        return INSTALL_OK;
    }
    const cJSON *provider = first_provider(request->catalog, request->chosen);
    if (provider == NULL) {
        return refuse_without_provider(reason, request);
    }
    return add_step(steps, provider_step(provider));
}

static install_status plan_with_package(cJSON *steps, const plan_request *request, const cJSON *mapping,
                                        refusal *reason) {
    install_status status = add_provider_step(steps, request, reason);
    return status == INSTALL_OK ? add_step(steps, install_step(request->chosen, mapping)) : status;
}

static install_status plan_through_provider(const cJSON *argument, const plan_request *request, cJSON *steps,
                                            refusal *reason) {
    cJSON *merged = NULL;
    install_status status = packages_of(argument, &merged, reason);
    if (status != INSTALL_OK) {
        return status;
    }
    const cJSON *mapping = member(merged, request->chosen);
    status = mapping == NULL ? refuse_without_package(reason, request)
                             : plan_with_package(steps, request, mapping, reason);
    cJSON_Delete(merged);
    return status;
}

static install_status plan_for(const cJSON *argument, const plan_request *request, cJSON *steps, refusal *reason) {
    if (strcmp(request->chosen, MARKETPLACE_METHOD) == 0) {
        return add_step(steps, install_step(MARKETPLACE_METHOD, NULL));
    }
    if (!lists_text(request->methods, request->chosen)) {
        return refuse_unlisted(reason, request);
    }
    return plan_through_provider(argument, request, steps, reason);
}

static install_status plan_of(const cJSON *argument, cJSON *steps, refusal *reason) {
    plan_request request;
    install_status status = read_plan_request(argument, &request, reason);
    return status == INSTALL_OK ? plan_for(argument, &request, steps, reason) : status;
}

RUNTIME_EXPORT int32_t runtime_install_plan(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t parsed = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (parsed != SPISOR_STATUS_SUCCESS) {
        return parsed;
    }
    refusal reason;
    cJSON *steps = cJSON_CreateArray();
    install_status status = steps == NULL ? INSTALL_OUT_OF_MEMORY : plan_of(call.argument, steps, &reason);
    int32_t answered = status == INSTALL_OK ? answer_member("steps", steps, out, out_len)
                                            : answer_failure(status, &reason, out, out_len);
    if (status != INSTALL_OK) {
        cJSON_Delete(steps);
    }
    spisor_discard_call(&call);
    return answered;
}
