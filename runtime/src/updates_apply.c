#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_installed.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "plugin_choices.h"
#include "run_records.h"
#include "updates_catalog.h"
#include "updates_request.h"
#include "version_map.h"

/** What a start resolved before it takes the lock: the merged catalog and its plugins' {run, fetch}. */
typedef struct start_plan {
    cJSON *catalog;
    cJSON *plugins;
} start_plan;

/** The references a start records, and the host version it runs, borrowed from them. */
typedef struct start_record {
    runtime_host_references references;
    const char *version;
} start_record;

static int plan_start(const runtime_updates_request *request, start_plan *plan) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(request->argument, "settings");
    cJSON *sources = runtime_updates_sources(settings);
    plan->catalog = sources == NULL ? NULL : runtime_updates_current_catalog(sources, request->cache);
    cJSON *candidates =
        runtime_plugin_candidates(settings, cJSON_GetObjectItemCaseSensitive(request->argument, "defaults"));
    cJSON *installed = runtime_cache_installed_bundles(request->cache);
    plan->plugins = plan->catalog == NULL || candidates == NULL || installed == NULL
                        ? NULL
                        : runtime_updates_resolve(plan->catalog, candidates, installed, "plugin");
    cJSON_Delete(installed);
    cJSON_Delete(candidates);
    cJSON_Delete(sources);
    return plan->plugins != NULL;
}

static void free_plan(start_plan *plan) {
    cJSON_Delete(plan->catalog);
    cJSON_Delete(plan->plugins);
}

static const cJSON *running_of(const runtime_updates_request *request) {
    const cJSON *running = cJSON_GetObjectItemCaseSensitive(request->argument, "running");
    return cJSON_IsObject(running) ? running : NULL;
}

/**
 * The version of the host this start runs: the one its references schedule once installed intact, else
 * the one they record it running, else the one the caller runs (cache-session.ts hostRunOf).
 */
static int switch_host(const runtime_updates_request *request, const runtime_host_references *recorded,
                       start_record *record) {
    const char *current = runtime_map_version(recorded->run, request->host_id);
    current = current != NULL ? current : runtime_map_version(running_of(request), request->host_id);
    const char *scheduled = runtime_map_version(recorded->next, request->host_id);
    int switching = scheduled != NULL && (current == NULL || strcmp(scheduled, current) != 0) &&
                    runtime_bundle_is_intact(request->cache, request->host_id, scheduled);
    const char *version = switching ? scheduled : current;
    record->references.next = cJSON_Duplicate(recorded->next, 1);
    record->references.rollback = cJSON_CreateObject();
    record->references.run = cJSON_CreateObject();
    if (record->references.next == NULL || record->references.rollback == NULL || record->references.run == NULL) {
        return 0;
    }
    if (scheduled != NULL && version != NULL && strcmp(scheduled, version) == 0) {
        cJSON_DeleteItemFromObjectCaseSensitive(record->references.next, request->host_id);
    }
    if (switching && current != NULL && !runtime_map_set(record->references.rollback, request->host_id, current)) {
        return 0;
    }
    return version == NULL || runtime_map_set(record->references.run, request->host_id, version);
}

static int add_fetches(cJSON *next, const cJSON *fetch) {
    int added = 1;
    for (const cJSON *target = fetch == NULL ? NULL : fetch->child; added && target != NULL; target = target->next) {
        added = runtime_map_set(next, cJSON_GetObjectItemCaseSensitive(target, "id")->valuestring,
                                cJSON_GetObjectItemCaseSensitive(target, "version")->valuestring);
    }
    return added;
}

/** run is the host's version, then what the caller runs beside it, then its plugins, as startFromCache orders them. */
static int complete_run(const runtime_updates_request *request, const start_plan *plan, start_record *record) {
    int completed = 1;
    for (const cJSON *entry = running_of(request) == NULL ? NULL : running_of(request)->child; completed && entry != NULL;
         entry = entry->next) {
        completed = !cJSON_IsString(entry) || strcmp(entry->string, request->host_id) == 0 ||
                    runtime_map_set(record->references.run, entry->string, entry->valuestring);
    }
    completed = completed &&
                    runtime_map_merge(record->references.run, cJSON_GetObjectItemCaseSensitive(plan->plugins, "run")) &&
                    add_fetches(record->references.next, cJSON_GetObjectItemCaseSensitive(plan->plugins, "fetch"));
    record->version = runtime_map_version(record->references.run, request->host_id);
    return completed;
}

/** The libraries the next start boots, in the order the host names them, or NULL when one is not installed. */
static cJSON *booted_libraries(const cJSON *ids, const cJSON *resolved) {
    cJSON *libraries = cJSON_CreateObject();
    for (const cJSON *id = ids->child; libraries != NULL && id != NULL; id = id->next) {
        const char *version = cJSON_IsString(id) ? runtime_map_version(resolved, id->valuestring) : NULL;
        if (cJSON_IsString(id) && (version == NULL || !runtime_map_set(libraries, id->valuestring, version))) {
            cJSON_Delete(libraries);
            libraries = NULL;
        }
    }
    return libraries;
}

static int schedule_changed(cJSON *next, const cJSON *running, const cJSON *resolved) {
    int scheduled = 1;
    for (const cJSON *entry = resolved == NULL ? NULL : resolved->child; scheduled && entry != NULL;
         entry = entry->next) {
        const char *loaded = runtime_map_version(running, entry->string);
        if (loaded == NULL || strcmp(loaded, entry->valuestring) != 0) {
            scheduled = runtime_map_set(next, entry->string, entry->valuestring);
        }
    }
    return scheduled;
}

/**
 * Resolves the libraries the host boots with for its next start and records them in its boot hint with
 * the host version this start runs (boot-from-cache.ts hostLibraryReferences).
 *
 * @implNote A hint naming a library that is not installed would refuse every boot, so no hint is written
 * until each one the host names resolves to an installed version.
 */
static int hint_libraries(const runtime_updates_request *request, const start_plan *plan, start_record *record,
                          cJSON **refusal) {
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(request->argument, "libraries");
    if (!cJSON_IsArray(ids)) {
        return 1;
    }
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(request->argument, "settings");
    cJSON *candidates = runtime_pinned_candidates(ids, settings);
    cJSON *installed = runtime_cache_installed_libraries(request->cache, request->platform);
    cJSON *resolution = candidates == NULL || installed == NULL
                            ? NULL
                            : runtime_updates_resolve(plan->catalog, candidates, installed, "library");
    const cJSON *resolved = cJSON_GetObjectItemCaseSensitive(resolution, "run");
    cJSON *libraries = resolved == NULL ? NULL : booted_libraries(ids, resolved);
    int hinted = resolved != NULL && schedule_changed(record->references.next, running_of(request), resolved);
    char *problem = NULL;
    if (hinted && libraries != NULL &&
        !runtime_write_boot_hint(request->cache, request->host_id, libraries, record->version, &problem)) {
        *refusal = runtime_updates_unrecorded(problem);
        hinted = 0;
    } else {
        free(problem);
    }
    cJSON_Delete(libraries);
    cJSON_Delete(resolution);
    cJSON_Delete(installed);
    cJSON_Delete(candidates);
    return hinted;
}

static int record_start(const runtime_updates_request *request, const start_plan *plan, start_record *record,
                        cJSON **refusal) {
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    int built = recorded.run != NULL && switch_host(request, &recorded, record) && complete_run(request, plan, record) &&
                hint_libraries(request, plan, record, refusal);
    runtime_free_host_references(&recorded);
    char *problem = NULL;
    if (built && !runtime_record_host_references(request->state, request->host_id, &record->references, &problem)) {
        *refusal = runtime_updates_unrecorded(problem);
        return 0;
    }
    return built;
}

static cJSON *started(const start_plan *plan, start_record *record, char *run_record) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && run_record != NULL &&
                runtime_catalog_attach(answer, "plugins",
                                       cJSON_DetachItemFromObjectCaseSensitive(plan->plugins, "run")) &&
                cJSON_AddItemReferenceToObject(answer, "run", record->references.run) &&
                cJSON_AddItemReferenceToObject(answer, "next", record->references.next) &&
                cJSON_AddItemReferenceToObject(answer, "rollback", record->references.rollback) &&
                cJSON_AddStringToObject(answer, "runRecord", run_record) != NULL;
    cJSON *copy = built ? cJSON_Duplicate(answer, 1) : NULL;
    cJSON_Delete(answer);
    return copy;
}

static char *unrecorded_run(const runtime_updates_request *request) {
    const char *parts[4] = {"the run record of this start of ", request->host_id, " could not be written under ",
                            request->state};
    return runtime_cache_text(parts, 4);
}

cJSON *runtime_updates_apply_for(const runtime_updates_request *request, cJSON **refusal) {
    start_plan plan = {NULL, NULL};
    if (!plan_start(request, &plan)) {
        free_plan(&plan);
        return NULL;
    }
    start_record record = {{NULL, NULL, NULL}, NULL};
    runtime_hosts_guard *guard = runtime_updates_lock(request, refusal);
    int recorded = guard != NULL && record_start(request, &plan, &record, refusal);
    runtime_hosts_unlock(guard);
    char *run_record = recorded ? runtime_record_run(request->state, request->host_id, record.references.run,
                                                     RUNTIME_DEFAULT_CHECK_MILLISECONDS)
                                : NULL;
    cJSON *answer = run_record == NULL ? NULL : started(&plan, &record, run_record);
    if (recorded && run_record == NULL) {
        *refusal = runtime_updates_unrecorded(unrecorded_run(request));
    }
    free(run_record);
    runtime_free_host_references(&record.references);
    free_plan(&plan);
    return answer;
}
