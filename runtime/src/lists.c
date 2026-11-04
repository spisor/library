#include "lists.h"

#include <stdlib.h>
#include <string.h>

#include "json_service.h"
#include "on_path.h"
#include "paths.h"
#include "registry.h"
#include "spisor/json_kind.h"

static const char *const STATES[3] = {"installed", "detected", "available"};

static const char *id_of(const cJSON *row) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "id"));
}

static int is_row_list(const cJSON *rows) {
    if (!cJSON_IsArray(rows)) {
        return 0;
    }
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        if (!cJSON_IsObject(row) || id_of(row) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int is_listed(const cJSON *list, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, list) {
        if (strcmp(id_of(row), id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int by_id(const void *left, const void *right) {
    return strcmp(id_of(*(const cJSON *const *)left), id_of(*(const cJSON *const *)right));
}

/** The rows of group no earlier group listed, first occurrences only, as borrowed pointers. */
static const cJSON **fresh_rows(const cJSON *group, const cJSON *list, size_t *count) {
    int size = cJSON_GetArraySize(group);
    const cJSON **rows = malloc((size_t)(size > 0 ? size : 1) * sizeof *rows);
    *count = 0;
    const cJSON *source = rows == NULL ? NULL : group;
    const cJSON *row;
    cJSON_ArrayForEach(row, source) {
        int seen = is_listed(list, id_of(row));
        for (size_t index = 0; !seen && index < *count; index++) {
            seen = strcmp(id_of(rows[index]), id_of(row)) == 0;
        }
        if (!seen) {
            rows[(*count)++] = row;
        }
    }
    return rows;
}

static int add_with_state(cJSON *list, const cJSON *row, const char *state) {
    cJSON *copy = cJSON_Duplicate(row, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "state");
    if (copy == NULL || cJSON_AddStringToObject(copy, "state", state) == NULL || !cJSON_AddItemToArray(list, copy)) {
        cJSON_Delete(copy);
        return 0;
    }
    return 1;
}

static int add_group(cJSON *list, const cJSON *group, const char *state) {
    size_t count = 0;
    const cJSON **rows = fresh_rows(group, list, &count);
    int complete = rows != NULL;
    if (complete) {
        qsort(rows, count, sizeof *rows, by_id);
    }
    for (size_t index = 0; complete && index < count; index++) {
        complete = add_with_state(list, rows[index], state);
    }
    free(rows);
    return complete;
}

cJSON *runtime_three_state_list(const cJSON *installed, const cJSON *detected, const cJSON *available) {
    const cJSON *groups[3] = {installed, detected, available};
    cJSON *list = cJSON_CreateArray();
    for (size_t index = 0; list != NULL && index < 3; index++) {
        if (!add_group(list, groups[index], STATES[index])) {
            cJSON_Delete(list);
            list = NULL;
        }
    }
    return list;
}

static cJSON *items_answer(cJSON *items) {
    cJSON *answer = items == NULL ? NULL : cJSON_CreateObject();
    if (answer == NULL || !cJSON_AddItemToObject(answer, "items", items)) {
        cJSON_Delete(answer);
        cJSON_Delete(items);
        return NULL;
    }
    return answer;
}

static cJSON *plugins_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *installed = cJSON_GetObjectItemCaseSensitive(argument, "installed");
    const cJSON *detected = cJSON_GetObjectItemCaseSensitive(argument, "detected");
    const cJSON *available = cJSON_GetObjectItemCaseSensitive(argument, "available");
    if (!is_row_list(installed) || !is_row_list(detected) || !is_row_list(available)) {
        *refusal = runtime_refusal("the argument carries no usable installed, detected or available rows",
                                   "send each of installed, detected and available as an array of {\"id\": "
                                   "\"<plugin id>\", ...} rows");
        return NULL;
    }
    return items_answer(runtime_three_state_list(installed, detected, available));
}

RUNTIME_EXPORT int32_t runtime_lists_plugins(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, plugins_for);
}

/** The registered hosts' application ids, as an array of rows { id }; NULL with *refusal when the registry is unreadable. */
static cJSON *registered_rows(const cJSON *places, cJSON **refusal) {
    cJSON *request = cJSON_CreateObject();
    if (request != NULL && !cJSON_AddItemToObject(request, "places", cJSON_Duplicate(places, 1))) {
        cJSON_Delete(request);
        request = NULL;
    }
    cJSON *listed = runtime_call_or_refuse(runtime_registry_list, request, refusal);
    cJSON *rows = listed == NULL ? NULL : cJSON_CreateArray();
    const cJSON *hosts = rows == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(listed, "hosts");
    const cJSON *entry;
    cJSON_ArrayForEach(entry, hosts) {
        const cJSON *application = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(entry, "registration"), "application");
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(application, "id"));
        cJSON *row = id == NULL ? NULL : cJSON_CreateObject();
        if (row != NULL && (cJSON_AddStringToObject(row, "id", id) == NULL || !cJSON_AddItemToArray(rows, row))) {
            cJSON_Delete(row);
        }
    }
    cJSON_Delete(listed);
    return rows;
}

static int reports_package_installed(const cJSON *detections, const char *id) {
    const cJSON *detection;
    cJSON_ArrayForEach(detection, detections) {
        const char *item = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(detection, "item"));
        const cJSON *answer = cJSON_GetObjectItemCaseSensitive(detection, "answer");
        int refused = cJSON_GetObjectItemCaseSensitive(detection, "refused") != NULL;
        if (item != NULL && strcmp(item, id) == 0 && !refused &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "installed"))) {
            return 1;
        }
    }
    return 0;
}

static int is_detected(const cJSON *application, const cJSON *environment, const cJSON *detections) {
    const char *binary = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(application, "binary"));
    char *found = binary == NULL ? NULL : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, binary, environment);
    int detected = found != NULL || reports_package_installed(detections, id_of(application));
    free(found);
    return detected;
}

static int add_row(cJSON *rows, const cJSON *application) {
    cJSON *row = cJSON_CreateObject();
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(application, "label"));
    int added = row != NULL && cJSON_AddStringToObject(row, "id", id_of(application)) != NULL &&
                (label == NULL || cJSON_AddStringToObject(row, "label", label) != NULL) && cJSON_AddItemToArray(rows, row);
    if (!added) {
        cJSON_Delete(row);
    }
    return added;
}

/** The catalog's applications as available and detected rows, plus installed ones that a registration names. */
typedef struct application_groups {
    cJSON *installed;
    cJSON *detected;
    cJSON *available;
} application_groups;

static int sort_application(application_groups *groups, const cJSON *registered, const cJSON *application,
                            const cJSON *environment, const cJSON *detections) {
    int complete = add_row(groups->available, application);
    if (complete && is_listed(registered, id_of(application))) {
        complete = add_row(groups->installed, application);
    }
    if (complete && is_detected(application, environment, detections)) {
        complete = add_row(groups->detected, application);
    }
    return complete;
}

static int is_host_application(const cJSON *item) {
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "kind"));
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(item, "application");
    return kind != NULL && strcmp(kind, "host") == 0 && cJSON_IsObject(application) && id_of(application) != NULL;
}

static cJSON *application_rows(const cJSON *items, const cJSON *registered, const cJSON *environment,
                               const cJSON *detections) {
    application_groups groups = {cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateArray()};
    int complete = groups.installed != NULL && groups.detected != NULL && groups.available != NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (complete && is_host_application(item)) {
            complete = sort_application(&groups, registered, cJSON_GetObjectItemCaseSensitive(item, "application"),
                                        environment, detections);
        }
    }
    cJSON *list = complete ? runtime_three_state_list(groups.installed, groups.detected, groups.available) : NULL;
    cJSON_Delete(groups.installed);
    cJSON_Delete(groups.detected);
    cJSON_Delete(groups.available);
    return list;
}

static cJSON *refuse_applications(cJSON **refusal) {
    *refusal = runtime_refusal("the argument carries no usable catalog, places, environment or detections",
                               "send {\"catalog\": {\"items\": {...}}, the catalog catalog.read answers, \"places\": "
                               "{\"state\": \"<absolute directory>\"}, \"environment\": {<name>: <value>}, "
                               "\"detections\": [{\"method\", \"item\", \"package\", \"answer\": {\"installed\"}} or "
                               "{\"method\", \"item\", \"refused\"}]}");
    return NULL;
}

static cJSON *applications_for(const cJSON *argument, cJSON **refusal) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(argument, "catalog"), "items");
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    const cJSON *detections = cJSON_GetObjectItemCaseSensitive(argument, "detections");
    if (!cJSON_IsObject(items) || !cJSON_IsObject(places) || !cJSON_IsObject(environment) || !cJSON_IsArray(detections)) {
        return refuse_applications(refusal);
    }
    cJSON *registered = registered_rows(places, refusal);
    cJSON *answer = registered == NULL ? NULL : items_answer(application_rows(items, registered, environment, detections));
    cJSON_Delete(registered);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_lists_applications(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, applications_for);
}
