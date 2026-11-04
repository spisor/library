#include "install_report.h"

#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "cache_refusal.h"
#include "install_support.h"
#include "json_service.h"

static const char *const REPORT_MEMBERS[] = {"installed", "cached", "unavailable", "enabled",
                                             "applications", "loaders", "missing"};

cJSON *runtime_install_new_report(void) {
    cJSON *report = cJSON_CreateObject();
    for (size_t index = 0; report != NULL && index < sizeof REPORT_MEMBERS / sizeof REPORT_MEMBERS[0]; index++) {
        if (cJSON_AddArrayToObject(report, REPORT_MEMBERS[index]) == NULL) {
            cJSON_Delete(report);
            report = NULL;
        }
    }
    return report;
}

int runtime_install_report_unavailable(cJSON *report, const char *id, const char *message, const char *fix) {
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", id) == NULL ||
        cJSON_AddStringToObject(row, "message", message) == NULL || cJSON_AddStringToObject(row, "fix", fix) == NULL ||
        !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), row)) {
        cJSON_Delete(row);
        return 0;
    }
    return 1;
}

int runtime_install_report_refusal(cJSON *report, const char *id, cJSON *refusal) {
    int recorded = runtime_is_refusal(refusal) &&
                   runtime_install_report_unavailable(report, id, runtime_install_text(refusal, "message"),
                                                      runtime_install_text(refusal, "fix"));
    cJSON_Delete(refusal);
    return recorded;
}

static const cJSON *item_of(const cJSON *catalog, const char *id, const char *kind) {
    const cJSON *item = id == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), id);
    const char *item_kind = runtime_install_text(item, "kind");
    return item_kind != NULL && strcmp(item_kind, kind) == 0 ? item : NULL;
}

const char *runtime_install_target_version(const cJSON *catalog, const cJSON *target, const char *kind) {
    const cJSON *item = item_of(catalog, runtime_install_text(target, "id"), kind);
    const char *pin = runtime_install_text(target, "pin");
    return item == NULL ? NULL : pin != NULL ? pin : runtime_install_text(item, "latest");
}

static cJSON *install_call(const cJSON *item, const char *id, const char *version, const char *cache) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || cJSON_AddStringToObject(call, "cache", cache) == NULL ||
        cJSON_AddStringToObject(call, "id", id) == NULL || !runtime_install_copy_into(call, "item", item) ||
        cJSON_AddStringToObject(call, "version", version) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static int record_installed(cJSON *report, const char *id, const char *version, const cJSON *answer) {
    if (cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer, "fetched")) == 0) {
        const char *parts[3] = {id, "@", version};
        char *name = runtime_cache_text(parts, 3);
        int added = name != NULL && runtime_install_add_text(cJSON_GetObjectItemCaseSensitive(report, "cached"), name);
        free(name);
        return added;
    }
    cJSON *row = cJSON_CreateObject();
    if (row == NULL || cJSON_AddStringToObject(row, "id", id) == NULL ||
        cJSON_AddStringToObject(row, "version", version) == NULL ||
        !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(report, "installed"), row)) {
        cJSON_Delete(row);
        return 0;
    }
    return 1;
}

static int record_absent(cJSON *report, const char *id, const char *kind) {
    const char *parts[4] = {"no catalog source lists a ", kind, " ", id};
    char *message = runtime_cache_text(parts, 4);
    int recorded = message != NULL &&
                   runtime_install_report_unavailable(report, id, message, "add a catalog source that lists it, or leave it out");
    free(message);
    return recorded;
}

const char *runtime_install_item(const cJSON *catalog, const cJSON *target, const char *kind, const char *cache,
                                 cJSON *report, int *out_of_memory) {
    const char *id = runtime_install_text(target, "id");
    const cJSON *item = item_of(catalog, id, kind);
    const char *version = runtime_install_target_version(catalog, target, kind);
    if (item == NULL || version == NULL) {
        *out_of_memory = id == NULL || !record_absent(report, id, kind);
        return NULL;
    }
    cJSON *refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_cache_install, install_call(item, id, version, cache), &refusal);
    if (answer == NULL) {
        *out_of_memory = !runtime_install_report_refusal(report, id, refusal);
        return NULL;
    }
    *out_of_memory = !record_installed(report, id, version, answer);
    cJSON_Delete(answer);
    return *out_of_memory ? NULL : version;
}

cJSON *runtime_install_target(const char *id) {
    cJSON *target = cJSON_CreateObject();
    if (target == NULL || cJSON_AddStringToObject(target, "id", id) == NULL) {
        cJSON_Delete(target);
        return NULL;
    }
    return target;
}
