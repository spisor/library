#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_installed.h"
#include "cache_refusal.h"
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

/**
 * What a start records: its references, the host version it runs (borrowed from them), the plugin versions
 * it runs, and what it could not record without that stopping the start.
 */
typedef struct start_record {
    runtime_host_references references;
    const char *version;
    cJSON *plugins;
    cJSON *unrecorded;
} start_record;

static int plan_start(const runtime_updates_request *request, start_plan *plan, cJSON **refusal) {
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(request->argument, "settings");
    cJSON *sources = runtime_updates_sources(settings, refusal);
    plan->catalog = sources == NULL ? NULL : runtime_updates_current_catalog(sources, request->cache, refusal);
    cJSON *candidates =
        runtime_plugin_candidates(settings, cJSON_GetObjectItemCaseSensitive(request->argument, "defaults"));
    cJSON *installed = runtime_cache_installed_bundles(request->cache);
    plan->plugins = plan->catalog == NULL || candidates == NULL || installed == NULL
                        ? NULL
                        : runtime_updates_resolve(plan->catalog, candidates, installed, "plugin", refusal);
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

static int start_references(const runtime_host_references *recorded, start_record *record) {
    record->references.next = cJSON_Duplicate(recorded->next, 1);
    record->references.rollback = cJSON_CreateObject();
    record->references.run = cJSON_CreateObject();
    record->references.rolled_back_from =
        recorded->rolled_back_from == NULL ? NULL : cJSON_Duplicate(recorded->rolled_back_from, 1);
    return record->references.next != NULL && record->references.rollback != NULL &&
           record->references.run != NULL &&
           (recorded->rolled_back_from == NULL || record->references.rolled_back_from != NULL);
}

/**
 * The version of the host this start runs: the one its references schedule once installed intact, else
 * the one they record it running, else the one the caller runs.
 */
static int switch_host(const runtime_updates_request *request, const runtime_host_references *recorded,
                       start_record *record) {
    const char *current = runtime_map_version(recorded->run, request->host_id);
    current = current != NULL ? current : runtime_map_version(running_of(request), request->host_id);
    const char *scheduled = runtime_map_version(recorded->next, request->host_id);
    int switching = scheduled != NULL && (current == NULL || strcmp(scheduled, current) != 0) &&
                    runtime_bundle_is_intact(request->cache, request->host_id, scheduled);
    const char *version = switching ? scheduled : current;
    if (!start_references(recorded, record)) {
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

/** Schedules each fetch in next, leaving out a version a rollback moved away from. */
static int add_fetches(cJSON *next, const cJSON *fetch, const cJSON *rolled_back_from) {
    int added = 1;
    for (const cJSON *target = fetch == NULL ? NULL : fetch->child; added && target != NULL; target = target->next) {
        const char *id = cJSON_GetObjectItemCaseSensitive(target, "id")->valuestring;
        const char *version = cJSON_GetObjectItemCaseSensitive(target, "version")->valuestring;
        const char *rolled_away = runtime_map_version(rolled_back_from, id);
        added = (rolled_away != NULL && strcmp(rolled_away, version) == 0) || runtime_map_set(next, id, version);
    }
    return added;
}

/**
 * The version of a plugin this start runs: the resolved one, unless a rollback moved the plugin away from
 * exactly that version and the one it rolled back to is still installed intact, which then holds.
 */
static const char *plugin_version(const runtime_updates_request *request, const runtime_host_references *recorded,
                                  const char *id, const char *resolved) {
    const char *previous = runtime_map_version(recorded->run, id);
    const char *rolled_away = runtime_map_version(recorded->rolled_back_from, id);
    int holds = rolled_away != NULL && previous != NULL && strcmp(resolved, rolled_away) == 0 &&
                strcmp(previous, rolled_away) != 0 && runtime_bundle_is_intact(request->cache, id, previous);
    return holds ? previous : resolved;
}

/** Sets each plugin's version into run, keeping the version it ran before as its rollback when it changes. */
static int add_plugins(const runtime_updates_request *request, const runtime_host_references *recorded,
                       const cJSON *resolved, start_record *record) {
    int added = record->plugins != NULL;
    for (const cJSON *entry = resolved == NULL ? NULL : resolved->child; added && entry != NULL; entry = entry->next) {
        const char *version = plugin_version(request, recorded, entry->string, entry->valuestring);
        const char *previous = runtime_map_version(recorded->run, entry->string);
        added = runtime_map_set(record->plugins, entry->string, version) &&
                runtime_map_set(record->references.run, entry->string, version) &&
                (previous == NULL || strcmp(previous, version) == 0 ||
                 runtime_map_set(record->references.rollback, entry->string, previous));
    }
    return added;
}

/** run is the host's version, then what the caller runs beside it, then its plugins. */
static int complete_run(const runtime_updates_request *request, const runtime_host_references *recorded,
                        const start_plan *plan, start_record *record) {
    int completed = 1;
    const cJSON *running = running_of(request);
    for (const cJSON *entry = running == NULL ? NULL : running->child; completed && entry != NULL; entry = entry->next) {
        completed = !cJSON_IsString(entry) || strcmp(entry->string, request->host_id) == 0 ||
                    runtime_map_set(record->references.run, entry->string, entry->valuestring);
    }
    completed = completed &&
                add_plugins(request, recorded, cJSON_GetObjectItemCaseSensitive(plan->plugins, "run"), record) &&
                add_fetches(record->references.next, cJSON_GetObjectItemCaseSensitive(plan->plugins, "fetch"),
                            record->references.rolled_back_from);
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

/** Adds {message, fix} for what this start could not record, taking problem, which it frees. */
static int note_unrecorded(cJSON *unrecorded, char *problem) {
    cJSON *entry = runtime_updates_unrecorded(problem);
    if (entry == NULL || !cJSON_AddItemToArray(unrecorded, entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

/**
 * Resolves the libraries the host boots with for its next start and records them in its boot hint with
 * the host version this start runs.
 *
 * @implNote A hint naming a library that is not installed would refuse every boot, so no hint is written
 * until each one the host names resolves to an installed version. A hint that cannot be written is
 * reported in the answer's unrecorded, never refused: a step of a start must not stop the start.
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
                            : runtime_updates_resolve(plan->catalog, candidates, installed, "library", refusal);
    const cJSON *resolved = cJSON_GetObjectItemCaseSensitive(resolution, "run");
    cJSON *libraries = resolved == NULL ? NULL : booted_libraries(ids, resolved);
    int hinted = resolved != NULL && schedule_changed(record->references.next, running_of(request), resolved);
    char *problem = NULL;
    if (hinted && libraries != NULL &&
        !runtime_write_boot_hint(request->cache, request->host_id, libraries, record->version, &problem)) {
        hinted = note_unrecorded(record->unrecorded, problem);
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
    int built = recorded.run != NULL && switch_host(request, &recorded, record) &&
                complete_run(request, &recorded, plan, record) && hint_libraries(request, plan, record, refusal);
    runtime_free_host_references(&recorded);
    char *problem = NULL;
    if (built && !runtime_record_host_references(request->state, request->host_id, &record->references, &problem)) {
        *refusal = runtime_updates_unrecorded(problem);
        return 0;
    }
    return built;
}

static cJSON *started(start_record *record, const char *run_record) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddItemReferenceToObject(answer, "plugins", record->plugins) &&
                cJSON_AddItemReferenceToObject(answer, "run", record->references.run) &&
                cJSON_AddItemReferenceToObject(answer, "next", record->references.next) &&
                cJSON_AddItemReferenceToObject(answer, "rollback", record->references.rollback) &&
                cJSON_AddItemReferenceToObject(answer, "unrecorded", record->unrecorded) &&
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
    if (!plan_start(request, &plan, refusal)) {
        free_plan(&plan);
        return NULL;
    }
    start_record record = {{NULL, NULL, NULL, NULL}, NULL, cJSON_CreateObject(), cJSON_CreateArray()};
    runtime_hosts_guard *guard = record.unrecorded == NULL ? NULL : runtime_updates_lock_registered(request, refusal);
    int recorded = guard != NULL && record_start(request, &plan, &record, refusal);
    char *run_record = recorded ? runtime_record_run(request->state, request->host_id, record.references.run,
                                                     RUNTIME_DEFAULT_CHECK_MILLISECONDS)
                                : NULL;
    runtime_hosts_unlock(guard);
    cJSON *answer = run_record == NULL ? NULL : started(&record, run_record);
    if (recorded && run_record == NULL) {
        *refusal = runtime_updates_unrecorded(unrecorded_run(request));
    }
    free(run_record);
    runtime_free_host_references(&record.references);
    cJSON_Delete(record.plugins);
    cJSON_Delete(record.unrecorded);
    free_plan(&plan);
    return answer;
}
