#include "install_items.h"
#include "allocation.h"

#include "boot.h"
#include "hosts_layers.h"
#include "install_entries.h"
#include "install_references.h"
#include "install_report.h"
#include "install_runtime.h"
#include "install_support.h"
#include "plugin_choices.h"

static int note_running(runtime_install_items *items, const char *id, const char *version) {
    cJSON_DeleteItemFromObjectCaseSensitive(items->running, id);
    return cJSON_AddStringToObject(items->running, id, version) != NULL;
}

int runtime_install_referenced(runtime_install_items *items, const char *host_id, const cJSON *target,
                               const char *kind) {
    runtime_install_run *run = items->run;
    const char *id = runtime_install_text(target, "id");
    const char *version = runtime_install_target_version(run->catalog, target, kind);
    cJSON *versions = version == NULL ? NULL : cJSON_CreateObject();
    cJSON *refusal = NULL;
    int referenced = versions != NULL && cJSON_AddStringToObject(versions, id, version) != NULL &&
                     runtime_install_reference_versions(run->state, host_id, versions, &refusal);
    cJSON_Delete(versions);
    if (version != NULL && !referenced) {
        return runtime_install_report_refusal(run->report, id, refusal);
    }
    int out_of_memory = 0;
    const char *installed = runtime_install_item(run->catalog, target, kind, run->cache, run->report, &out_of_memory);
    return !out_of_memory && (installed == NULL || note_running(items, id, installed));
}

int runtime_install_pinned(runtime_install_items *items, const char *const *ids, size_t count, const char *kind) {
    cJSON *names = cJSON_CreateArray();
    for (size_t index = 0; names != NULL && index < count; index++) {
        if (!runtime_install_add_text(names, ids[index])) {
            cJSON_Delete(names);
            names = NULL;
        }
    }
    cJSON *targets = names == NULL ? NULL : runtime_pinned_candidates(names, items->values);
    int installed = targets != NULL;
    for (const cJSON *target = installed ? targets->child : NULL; installed && target != NULL; target = target->next) {
        installed = runtime_install_referenced(items, RUNTIME_ECOSYSTEM_HOST_ID, target, kind);
    }
    cJSON_Delete(targets);
    cJSON_Delete(names);
    return installed;
}

/** The running versions of the libraries, as an object of library id to version; NULL when memory runs out. */
static cJSON *hint_libraries(const runtime_install_items *items, const char *const *libraries, size_t count) {
    cJSON *versions = cJSON_CreateObject();
    int built = versions != NULL;
    for (size_t index = 0; built && index < count; index++) {
        built = runtime_install_copy_into(versions, libraries[index],
                                          cJSON_GetObjectItemCaseSensitive(items->running, libraries[index]));
    }
    if (!built) {
        cJSON_Delete(versions);
        return NULL;
    }
    return versions;
}

int runtime_install_write_hint(runtime_install_items *items, const char *host_id, const char *const *libraries,
                               size_t count) {
    runtime_install_run *run = items->run;
    if (!cJSON_HasObjectItem(items->running, host_id)) {
        return 1;
    }
    cJSON *refusal = NULL;
    cJSON *versions = hint_libraries(items, libraries, count);
    runtime_boot_hint_request request = {run->cache, host_id, versions, runtime_install_text(items->running, host_id),
                                         run->state, run->platform};
    int written = versions != NULL && runtime_boot_write(&request, &refusal);
    cJSON_Delete(versions);
    return written || (refusal != NULL && runtime_install_report_refusal(run->report, host_id, refusal));
}

int runtime_install_language_host(runtime_install_items *items, const char *language) {
    static const char *const libraries[2] = {RUNTIME_LIBRARY_ID, RUNTIME_FOREBAY_LIBRARY_ID};
    char *host_id = runtime_language_host_id(language);
    const char *host_ids[1] = {host_id};
    int installed = host_id != NULL && runtime_install_pinned(items, host_ids, 1, "language-host") &&
                    runtime_install_write_hint(items, host_id, libraries, 2);
    spisor_memory_release(host_id);
    return installed;
}

int runtime_install_acquire_javascript(runtime_install_run *run) {
    cJSON *refusal = NULL;
    cJSON *acquired = runtime_acquire_javascript(run->cache, run->platform, run->environment, &refusal);
    if (acquired == NULL) {
        return refusal != NULL && runtime_install_report_refusal(run->report, "node", refusal);
    }
    return cJSON_AddItemToObject(run->report, "javascript", acquired);
}
