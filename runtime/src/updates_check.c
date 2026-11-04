#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_install.h"
#include "cache_installed.h"
#include "cache_refusal.h"
#include "cache_sweep.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "plugin_choices.h"
#include "updates_catalog.h"
#include "updates_request.h"
#include "version_map.h"

#define DEFAULT_INTERVAL_HOURS 24.0

/** What one check works from: the sources it asked, the catalog they give, and the versions to install. */
typedef struct check_plan {
    cJSON *sources;
    cJSON *catalog;
    cJSON *targets;
    cJSON *hosts;
} check_plan;

/** What one check did, the arrays of its answer. */
typedef struct check_progress {
    cJSON *checked;
    cJSON *fetched;
    cJSON *refused;
} check_progress;

static const cJSON *settings_of(const runtime_updates_request *request) {
    return cJSON_GetObjectItemCaseSensitive(request->argument, "settings");
}

/** updates.intervalHours from the ecosystem settings, 24 when it is absent or not a positive number. */
static double interval_hours_of(const cJSON *settings) {
    const cJSON *updates = cJSON_GetObjectItemCaseSensitive(settings, "updates");
    const cJSON *configured = cJSON_IsObject(updates) ? cJSON_GetObjectItemCaseSensitive(updates, "intervalHours") : NULL;
    return cJSON_IsNumber(configured) && isfinite(configured->valuedouble) && configured->valuedouble > 0
               ? configured->valuedouble
               : DEFAULT_INTERVAL_HOURS;
}

static int check_sources(const runtime_updates_request *request, const cJSON *sources, const char *now,
                         cJSON *checked) {
    double interval_hours = interval_hours_of(settings_of(request));
    int added = 1;
    for (const cJSON *source = sources->child; added && source != NULL; source = source->next) {
        cJSON *check = runtime_updates_check_source(source, request->cache, now, interval_hours);
        added = check != NULL && cJSON_AddItemToArray(checked, check);
        if (!added) {
            cJSON_Delete(check);
        }
    }
    return added;
}

static int add_target(cJSON *targets, const char *id, const char *version) {
    for (const cJSON *target = targets->child; target != NULL; target = target->next) {
        if (strcmp(runtime_map_version(target, "id"), id) == 0 &&
            strcmp(runtime_map_version(target, "version"), version) == 0) {
            return 1;
        }
    }
    cJSON *target = cJSON_CreateObject();
    int built = target != NULL && cJSON_AddStringToObject(target, "id", id) != NULL &&
                cJSON_AddStringToObject(target, "version", version) != NULL;
    if (!built || !cJSON_AddItemToArray(targets, target)) {
        cJSON_Delete(target);
        return 0;
    }
    return 1;
}

static int add_targets(cJSON *targets, const cJSON *listed) {
    int added = 1;
    for (const cJSON *entry = listed == NULL ? NULL : listed->child; added && entry != NULL; entry = entry->next) {
        const char *id = runtime_map_version(entry, "id");
        const char *version = runtime_map_version(entry, "version");
        added = id == NULL || version == NULL || add_target(targets, id, version);
    }
    return added;
}

/**
 * Each host the start keeps current, at its pin or else the catalog's latest, where that differs from the
 * version running, installed or not.
 *
 * @implNote A start runs the version its references name rather than resolving a host bundle as it does
 * a plugin, so a host's new version must be scheduled even when it is already in the cache.
 */
static int add_host_targets(cJSON *targets, const cJSON *hosts, const cJSON *catalog, const cJSON *run) {
    int added = 1;
    for (const cJSON *host = hosts->child; added && host != NULL; host = host->next) {
        const char *id = runtime_map_version(host, "id");
        const cJSON *item = runtime_updates_item(catalog, id);
        const char *kind = runtime_map_version(item, "kind");
        const char *pin = runtime_map_version(host, "pin");
        const char *version = pin != NULL ? pin : runtime_map_version(item, "latest");
        const char *running = runtime_map_version(run, id);
        if (kind != NULL && strcmp(kind, "host") == 0 && version != NULL &&
            runtime_updates_lists(catalog, id, version) && (running == NULL || strcmp(running, version) != 0)) {
            added = add_target(targets, id, version);
        }
    }
    return added;
}

static cJSON *kept_hosts(const runtime_updates_request *request) {
    cJSON *ids = cJSON_CreateArray();
    int built = ids != NULL && cJSON_AddItemToArray(ids, cJSON_CreateString(request->host_id)) &&
                cJSON_AddItemToArray(ids, cJSON_CreateString(RUNTIME_FOREBAY_HOST_ID));
    cJSON *hosts = built ? runtime_pinned_candidates(ids, settings_of(request)) : NULL;
    cJSON_Delete(ids);
    return hosts;
}

static cJSON *plugin_fetches(const runtime_updates_request *request, const cJSON *catalog) {
    cJSON *candidates =
        runtime_plugin_candidates(settings_of(request), cJSON_GetObjectItemCaseSensitive(request->argument, "defaults"));
    cJSON *installed = runtime_cache_installed_bundles(request->cache);
    cJSON *resolved = candidates == NULL || installed == NULL
                          ? NULL
                          : runtime_updates_resolve(catalog, candidates, installed, "plugin");
    cJSON *fetch = cJSON_DetachItemFromObjectCaseSensitive(resolved, "fetch");
    cJSON_Delete(resolved);
    cJSON_Delete(installed);
    cJSON_Delete(candidates);
    return fetch;
}

static int plan_targets(const runtime_updates_request *request, check_plan *plan) {
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    cJSON *fetch = plugin_fetches(request, plan->catalog);
    plan->hosts = kept_hosts(request);
    plan->targets = cJSON_CreateArray();
    int planned = recorded.run != NULL && fetch != NULL && plan->hosts != NULL && plan->targets != NULL &&
                  add_targets(plan->targets, cJSON_GetObjectItemCaseSensitive(request->argument, "problems")) &&
                  add_targets(plan->targets, fetch) &&
                  add_host_targets(plan->targets, plan->hosts, plan->catalog, recorded.run);
    cJSON_Delete(fetch);
    runtime_free_host_references(&recorded);
    return planned;
}

/** Each target that is not what runs, set into next (background.ts nextOf). */
static int schedule(cJSON *next, const cJSON *targets, const cJSON *run) {
    int scheduled = 1;
    for (const cJSON *target = targets->child; scheduled && target != NULL; target = target->next) {
        const char *id = runtime_map_version(target, "id");
        const char *version = runtime_map_version(target, "version");
        const char *running = runtime_map_version(run, id);
        if (running == NULL || strcmp(running, version) != 0) {
            scheduled = runtime_map_set(next, id, version);
        }
    }
    return scheduled;
}

/** Adds the targets to next as the references hold them now, under the lock, so no concurrent write is lost. */
static int record_next(const runtime_updates_request *request, const cJSON *targets, cJSON **refusal) {
    runtime_hosts_guard *guard = runtime_updates_lock(request, refusal);
    runtime_host_references recorded =
        guard == NULL ? (runtime_host_references){NULL, NULL, NULL}
                      : runtime_read_host_references(request->state, request->host_id);
    int recorded_next = recorded.run != NULL && schedule(recorded.next, targets, recorded.run);
    char *problem = NULL;
    if (recorded_next && !runtime_record_host_references(request->state, request->host_id, &recorded, &problem)) {
        *refusal = runtime_updates_unrecorded(problem);
        recorded_next = 0;
    }
    runtime_free_host_references(&recorded);
    runtime_hosts_unlock(guard);
    return recorded_next;
}

static int add_refused(cJSON *refused, const char *name, const char *message, const char *fix) {
    cJSON *entry = cJSON_CreateObject();
    int built = entry != NULL && cJSON_AddStringToObject(entry, "id", name) != NULL &&
                cJSON_AddStringToObject(entry, "message", message) != NULL &&
                cJSON_AddStringToObject(entry, "fix", fix) != NULL;
    if (!built || !cJSON_AddItemToArray(refused, entry)) {
        cJSON_Delete(entry);
        return 0;
    }
    return 1;
}

static int is_kept_host(const cJSON *hosts, const char *id) {
    for (const cJSON *host = hosts->child; host != NULL; host = host->next) {
        if (strcmp(runtime_map_version(host, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Installs one target and, for a host the start keeps current, switches its boot hint to it once it is
 * installed intact.
 *
 * @implNote Every command boots the version its hint names, so the hint changes only after the install
 * completed. Both happen under the registry lock, so a sweep cannot remove the version between its files
 * being stored and its completion marker being written.
 */
static cJSON *install_target(const runtime_updates_request *request, const check_plan *plan, const char *id,
                             const char *version, const cJSON *run, cJSON **refused) {
    runtime_hosts_guard *guard = runtime_updates_lock(request, refused);
    cJSON *answer = guard == NULL ? NULL
                                  : runtime_cache_install_version(request->cache, id,
                                                                  runtime_updates_item(plan->catalog, id), version,
                                                                  request->platform, refused);
    const char *running = runtime_map_version(run, id);
    if (answer != NULL && is_kept_host(plan->hosts, id) && (running == NULL || strcmp(running, version) != 0)) {
        runtime_hint_host_version(request->cache, id, version);
    }
    runtime_hosts_unlock(guard);
    return answer;
}

static int install_one(const runtime_updates_request *request, const check_plan *plan, const cJSON *target,
                       const cJSON *run, check_progress *progress) {
    const char *id = runtime_map_version(target, "id");
    const char *version = runtime_map_version(target, "version");
    const char *name_parts[3] = {id, "@", version};
    char *name = runtime_cache_text(name_parts, 3);
    int done = name != NULL;
    if (done && runtime_updates_item(plan->catalog, id) == NULL) {
        const char *message_parts[2] = {"no catalog source lists ", id};
        char *message = runtime_cache_text(message_parts, 2);
        done = message != NULL &&
               add_refused(progress->refused, name, message, "add a catalog source that lists it, or disable it");
        free(message);
    } else if (done) {
        cJSON *refusal = NULL;
        cJSON *answer = install_target(request, plan, id, version, run, &refusal);
        done = answer != NULL ? cJSON_AddItemToArray(progress->fetched, cJSON_CreateString(name))
               : refusal != NULL
                   ? add_refused(progress->refused, name, runtime_map_version(refusal, "message"),
                                 runtime_map_version(refusal, "fix"))
                   : 0;
        cJSON_Delete(answer);
        cJSON_Delete(refusal);
    }
    free(name);
    return done;
}

static int install_targets(const runtime_updates_request *request, const check_plan *plan, check_progress *progress) {
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    int installed = recorded.run != NULL;
    for (const cJSON *target = plan->targets->child; installed && target != NULL; target = target->next) {
        installed = install_one(request, plan, target, recorded.run, progress);
    }
    runtime_free_host_references(&recorded);
    return installed;
}

/** Sweeps with the registry lock held, never through cache.sweep, which takes the lock itself. */
static cJSON *sweep(const runtime_updates_request *request, cJSON **refusal) {
    runtime_hosts_guard *guard = runtime_updates_lock(request, refusal);
    cJSON *removed = guard == NULL ? NULL : runtime_cache_sweep_versions(request->cache, request->state);
    runtime_hosts_unlock(guard);
    return removed;
}

static int plan_check(const runtime_updates_request *request, const char *now, check_plan *plan,
                      check_progress *progress) {
    plan->sources = runtime_updates_sources(settings_of(request));
    int checked = plan->sources != NULL && check_sources(request, plan->sources, now, progress->checked);
    plan->catalog = checked ? runtime_updates_current_catalog(plan->sources, request->cache) : NULL;
    return plan->catalog != NULL && plan_targets(request, plan);
}

static cJSON *answer_of(check_progress *progress, cJSON *removed) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && runtime_catalog_attach(answer, "checked", progress->checked);
    progress->checked = NULL;
    built = built && runtime_catalog_attach(answer, "fetched", progress->fetched);
    progress->fetched = NULL;
    built = built && runtime_catalog_attach(answer, "refused", progress->refused);
    progress->refused = NULL;
    if (!built || !runtime_catalog_attach(answer, "removed", removed)) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static void free_check(check_plan *plan, check_progress *progress) {
    cJSON_Delete(plan->sources);
    cJSON_Delete(plan->catalog);
    cJSON_Delete(plan->targets);
    cJSON_Delete(plan->hosts);
    cJSON_Delete(progress->checked);
    cJSON_Delete(progress->fetched);
    cJSON_Delete(progress->refused);
}

cJSON *runtime_updates_check_for(const runtime_updates_request *request, cJSON **refusal) {
    const char *now = runtime_catalog_text_member(request->argument, "now");
    if (now == NULL || now[0] == 0) {
        const char *parts[1] = {"the argument carries no now"};
        *refusal = runtime_cache_refusal(parts, 1, "pass now, the time of the check as toISOString writes it");
        return NULL;
    }
    check_plan plan = {NULL, NULL, NULL, NULL};
    check_progress progress = {cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateArray()};
    int done = progress.checked != NULL && progress.fetched != NULL && progress.refused != NULL &&
               plan_check(request, now, &plan, &progress) && record_next(request, plan.targets, refusal) &&
               install_targets(request, &plan, &progress);
    cJSON *removed = done ? sweep(request, refusal) : NULL;
    cJSON *answer = removed == NULL ? NULL : answer_of(&progress, removed);
    free_check(&plan, &progress);
    return answer;
}
