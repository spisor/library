#include <string.h>

#include "catalog.h"
#include "catalog_answer.h"
#include "catalog_semver.h"
#include "spisor/json_kind.h"

static const char *const KINDS[] = {"plugin", "host", "library"};

static const cJSON *item_of_kind(const cJSON *catalog, const char *id, const char *kind) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), id);
    const char *item_kind = runtime_catalog_text_member(item, "kind");
    return item_kind != NULL && strcmp(item_kind, kind) == 0 ? item : NULL;
}

static int lists(const cJSON *versions, const char *version) {
    for (const cJSON *entry = versions == NULL ? NULL : versions->child; entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, version) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *newest_of(const cJSON *versions) {
    const char *newest = NULL;
    for (const cJSON *entry = versions == NULL ? NULL : versions->child; entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry) &&
            (newest == NULL || runtime_catalog_compare_versions(entry->valuestring, newest) >= 0)) {
            newest = entry->valuestring;
        }
    }
    return newest;
}

static const char *unpinned_run(const cJSON *item, const cJSON *versions) {
    const char *latest = runtime_catalog_text_member(item, "latest");
    if (item != NULL && latest != NULL && lists(versions, latest)) {
        return latest;
    }
    return newest_of(versions);
}

static const char *wanted(const cJSON *item, const char *pin) {
    if (item == NULL) {
        return NULL;
    }
    const char *version = pin != NULL ? pin : runtime_catalog_text_member(item, "latest");
    const cJSON *listed = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "versions"),
                                                           version == NULL ? "" : version);
    return version != NULL && listed != NULL ? version : NULL;
}

static int set_run(cJSON *run, const char *id, const char *version) {
    cJSON *value = cJSON_CreateString(version);
    if (value == NULL) {
        return 0;
    }
    if (cJSON_GetObjectItemCaseSensitive(run, id) != NULL) {
        return cJSON_ReplaceItemInObjectCaseSensitive(run, id, value);
    }
    return runtime_catalog_attach(run, id, value);
}

static int add_fetch(cJSON *fetch, const char *id, const char *version) {
    cJSON *entry = cJSON_CreateObject();
    int added = entry != NULL && cJSON_AddStringToObject(entry, "id", id) != NULL &&
                cJSON_AddStringToObject(entry, "version", version) != NULL && cJSON_AddItemToArray(fetch, entry);
    if (!added) {
        cJSON_Delete(entry);
    }
    return added;
}

static int resolve_one(const cJSON *catalog, const cJSON *installed, const char *kind, const char *id,
                       const char *pin, cJSON *run, cJSON *fetch) {
    const cJSON *item = item_of_kind(catalog, id, kind);
    const cJSON *listed = cJSON_GetObjectItemCaseSensitive(installed, id);
    const cJSON *versions = cJSON_IsArray(listed) ? listed : NULL;
    const char *running = pin == NULL ? unpinned_run(item, versions) : lists(versions, pin) ? pin : NULL;
    if (running != NULL && !set_run(run, id, running)) {
        return 0;
    }
    const char *next = wanted(item, pin);
    return next == NULL || lists(versions, next) || add_fetch(fetch, id, next);
}

static int usable_enabled(const cJSON *enabled) {
    for (const cJSON *entry = enabled->child; entry != NULL; entry = entry->next) {
        const cJSON *pin = cJSON_GetObjectItemCaseSensitive(entry, "pin");
        if (runtime_catalog_text_member(entry, "id") == NULL || (pin != NULL && !cJSON_IsString(pin))) {
            return 0;
        }
    }
    return 1;
}

static const char *kind_of(const cJSON *argument) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(argument, "kind");
    if (kind == NULL) {
        return KINDS[0];
    }
    for (size_t index = 0; cJSON_IsString(kind) && index < sizeof KINDS / sizeof KINDS[0]; index++) {
        if (strcmp(kind->valuestring, KINDS[index]) == 0) {
            return KINDS[index];
        }
    }
    return NULL;
}

static cJSON *resolved(const cJSON *catalog, const cJSON *enabled, const cJSON *installed, const char *kind) {
    cJSON *result = cJSON_CreateObject();
    cJSON *run = cJSON_AddObjectToObject(result, "run");
    cJSON *fetch = cJSON_AddArrayToObject(result, "fetch");
    int built = run != NULL && fetch != NULL;
    for (const cJSON *entry = enabled->child; built && entry != NULL; entry = entry->next) {
        built = resolve_one(catalog, installed, kind, runtime_catalog_text_member(entry, "id"),
                            runtime_catalog_text_member(entry, "pin"), run, fetch);
    }
    if (!built) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static int32_t resolve(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(argument, "catalog");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(argument, "enabled");
    const cJSON *installed = cJSON_GetObjectItemCaseSensitive(argument, "installed");
    const char *kind = kind_of(argument);
    if (!cJSON_IsObject(catalog) || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(catalog, "items"))) {
        return runtime_catalog_refuse_argument("catalog", "a catalog with items, as catalog.read answers it", out,
                                               out_len);
    }
    if (!cJSON_IsArray(enabled) || !usable_enabled(enabled)) {
        return runtime_catalog_refuse_argument("enabled", "an array of {id, pin?}", out, out_len);
    }
    if (!cJSON_IsObject(installed)) {
        return runtime_catalog_refuse_argument("installed", "an object of each id's installed versions", out,
                                               out_len);
    }
    if (kind == NULL) {
        return runtime_catalog_refuse_argument("kind", "plugin, host or library, or leave it out", out, out_len);
    }
    return runtime_catalog_answer(resolved(catalog, enabled, installed, kind), out, out_len);
}

RUNTIME_EXPORT int32_t runtime_catalog_resolve(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = resolve(call.argument, out, out_len);
    spisor_discard_call(&call);
    return status;
}
