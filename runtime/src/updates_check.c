#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_install.h"
#include "cache_folders.h"
#include "cache_installed.h"
#include "cache_marker.h"
#include "cache_refusal.h"
#include "cache_sweep.h"
#include "catalog_answer.h"
#include "host_references.h"
#include "plugin_choices.h"
#include "updates_catalog.h"
#include "updates_request.h"
#include "version_map.h"

#define DEFAULT_INTERVAL_HOURS 24.0
#define HINT_FIX "check that SPISOR_CACHE names a writable folder; the next check switches the hint again"

/**
 * What one check works from: the sources it asked, the catalog they give, the versions to install, the
 * hosts it keeps current, and the versions a rollback moved away from, which it does not schedule.
 */
typedef struct check_plan {
    cJSON *sources;
    cJSON *catalog;
    cJSON *targets;
    cJSON *hosts;
    cJSON *rolled_back_from;
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
                         cJSON *checked, cJSON **refusal) {
    double interval_hours = interval_hours_of(settings_of(request));
    int added = 1;
    for (const cJSON *source = sources->child; added && source != NULL; source = source->next) {
        cJSON *check = runtime_updates_check_source(source, request->cache, now, interval_hours, refusal);
        added = check != NULL && cJSON_AddItemToArray(checked, check);
        if (!added) {
            cJSON_Delete(check);
        }
    }
    return added;
}

/** Adds id@version once; a version held, which a rollback moved away from, is left out. */
static int add_target(cJSON *targets, const char *id, const char *version, const cJSON *held) {
    const char *rolled_away = runtime_map_version(held, id);
    if (rolled_away != NULL && strcmp(rolled_away, version) == 0) {
        return 1;
    }
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

static int add_targets(cJSON *targets, const cJSON *listed, const cJSON *held) {
    int added = 1;
    for (const cJSON *entry = listed == NULL ? NULL : listed->child; added && entry != NULL; entry = entry->next) {
        const char *id = runtime_map_version(entry, "id");
        const char *version = runtime_map_version(entry, "version");
        added = id == NULL || version == NULL || add_target(targets, id, version, held);
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
static int add_host_targets(cJSON *targets, const check_plan *plan, const cJSON *run) {
    int added = 1;
    for (const cJSON *host = plan->hosts->child; added && host != NULL; host = host->next) {
        const char *id = runtime_map_version(host, "id");
        const cJSON *item = runtime_updates_item(plan->catalog, id);
        const char *kind = runtime_map_version(item, "kind");
        const char *pin = runtime_map_version(host, "pin");
        const char *version = pin != NULL ? pin : runtime_map_version(item, "latest");
        const char *running = runtime_map_version(run, id);
        if (kind != NULL && strcmp(kind, "host") == 0 && version != NULL &&
            runtime_updates_lists(plan->catalog, id, version) && (running == NULL || strcmp(running, version) != 0)) {
            added = add_target(targets, id, version, plan->rolled_back_from);
        }
    }
    return added;
}

/** Whether check, which it frees, answers damaged. */
static int is_damaged(cJSON *check) {
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(check, "kind");
    int damaged = cJSON_IsString(kind) && strcmp(kind->valuestring, "damaged") == 0;
    cJSON_Delete(check);
    return damaged;
}

/** Whether the installed bundle version has a file whose size or hash differs from the marker; an absent version has not. */
static int is_damaged_on_disk(const char *cache, const char *id, const char *version) {
    const char *segments[4] = {cache, "bundles", id, version};
    char *directory = runtime_cache_joined(segments, 4);
    int damaged = directory != NULL && (is_damaged(runtime_cache_check_version_sizes(directory)) ||
                                        is_damaged(runtime_cache_check_version(directory)));
    free(directory);
    return damaged;
}

/**
 * Each version run names that is damaged on disk, by size or by hash, to install again.
 *
 * @implNote A start reports no problem of a native host it cannot boot, since nothing of it runs, so the
 * check finds them itself; a marker from before sizes were recorded is repaired this way. A start keeps its
 * size-only check to stay fast, so this hourly scan is what finds damage that kept every size.
 */
static int add_unstartable_targets(cJSON *targets, const char *cache, const cJSON *run) {
    int added = 1;
    for (const cJSON *entry = run == NULL ? NULL : run->child; added && entry != NULL; entry = entry->next) {
        if (cJSON_IsString(entry) && is_damaged_on_disk(cache, entry->string, entry->valuestring)) {
            added = add_target(targets, entry->string, entry->valuestring, NULL);
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

static cJSON *plugin_fetches(const runtime_updates_request *request, const cJSON *catalog, cJSON **refusal) {
    cJSON *candidates =
        runtime_plugin_candidates(settings_of(request), cJSON_GetObjectItemCaseSensitive(request->argument, "defaults"));
    cJSON *installed = runtime_cache_installed_bundles(request->cache);
    cJSON *resolved = candidates == NULL || installed == NULL
                          ? NULL
                          : runtime_updates_resolve(catalog, candidates, installed, "plugin", refusal);
    cJSON *fetch = cJSON_DetachItemFromObjectCaseSensitive(resolved, "fetch");
    cJSON_Delete(resolved);
    cJSON_Delete(installed);
    cJSON_Delete(candidates);
    return fetch;
}

static int plan_targets(const runtime_updates_request *request, check_plan *plan, cJSON **refusal) {
    runtime_host_references recorded = runtime_read_host_references(request->state, request->host_id);
    cJSON *fetch = plugin_fetches(request, plan->catalog, refusal);
    plan->hosts = kept_hosts(request);
    plan->targets = cJSON_CreateArray();
    plan->rolled_back_from = recorded.rolled_back_from;
    recorded.rolled_back_from = NULL;
    int planned = recorded.run != NULL && fetch != NULL && plan->hosts != NULL && plan->targets != NULL &&
                  add_targets(plan->targets, cJSON_GetObjectItemCaseSensitive(request->argument, "problems"), NULL) &&
                  add_unstartable_targets(plan->targets, request->cache, recorded.run) &&
                  add_targets(plan->targets, fetch, plan->rolled_back_from) &&
                  add_host_targets(plan->targets, plan, recorded.run);
    cJSON_Delete(fetch);
    runtime_free_host_references(&recorded);
    return planned;
}

/**
 * Each target that is not what runs, set into next, leaving out a version a rollback
 * moved away from as the references hold it now, under the lock.
 */
static int schedule(cJSON *next, const cJSON *targets, const runtime_host_references *recorded) {
    int scheduled = 1;
    for (const cJSON *target = targets->child; scheduled && target != NULL; target = target->next) {
        const char *id = runtime_map_version(target, "id");
        const char *version = runtime_map_version(target, "version");
        const char *running = runtime_map_version(recorded->run, id);
        const char *rolled_away = runtime_map_version(recorded->rolled_back_from, id);
        int held = rolled_away != NULL && strcmp(rolled_away, version) == 0;
        if (!held && (running == NULL || strcmp(running, version) != 0)) {
            scheduled = runtime_map_set(next, id, version);
        }
    }
    return scheduled;
}

/** Forgets each version a rollback moved away from once the catalog's latest of that item is another. */
static void release_rollbacks(cJSON *rolled_back_from, const cJSON *catalog) {
    cJSON *entry = rolled_back_from == NULL ? NULL : rolled_back_from->child;
    while (entry != NULL) {
        cJSON *following = entry->next;
        const char *latest = runtime_map_version(runtime_updates_item(catalog, entry->string), "latest");
        if (cJSON_IsString(entry) && latest != NULL && strcmp(latest, entry->valuestring) != 0) {
            cJSON_Delete(cJSON_DetachItemViaPointer(rolled_back_from, entry));
        }
        entry = following;
    }
}

/**
 * Adds the targets to next as the references hold them now, under the lock, so no concurrent write is
 * lost, and releases the rollbacks the catalog has moved past.
 */
static int record_next(const runtime_updates_request *request, const check_plan *plan, cJSON **refusal) {
    spisor_lock *guard = runtime_updates_lock_registered(request, refusal);
    runtime_host_references recorded =
        guard == NULL ? (runtime_host_references){NULL, NULL, NULL, NULL}
                      : runtime_read_host_references(request->state, request->host_id);
    release_rollbacks(recorded.rolled_back_from, plan->catalog);
    int recorded_next = recorded.run != NULL && schedule(recorded.next, plan->targets, &recorded);
    char *problem = NULL;
    if (recorded_next && !runtime_record_host_references(request->state, request->host_id, &recorded, &problem)) {
        *refusal = runtime_updates_unrecorded(problem);
        recorded_next = 0;
    }
    runtime_free_host_references(&recorded);
    spisor_unlock(guard);
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

static int names(const cJSON *map, const char *id, const char *version) {
    const char *named = runtime_map_version(map, id);
    return named != NULL && strcmp(named, version) == 0;
}

typedef enum hint_switch { HINT_UNCHANGED, HINT_SWITCHED, HINT_UNWRITTEN } hint_switch;

/**
 * Switches a kept-current host's boot hint to version once it is installed, when the host's references
 * still schedule or run it and it is still intact.
 *
 * @implNote Taken under the registry lock, briefly, after the install: the references are read again so
 * a version a later rollback or sweep let go is never hinted, and every command boots what the hint names.
 * The version's files are hashed before the lock is taken and only their sizes checked again under it.
 */
static hint_switch switch_hint(const runtime_updates_request *request, const char *id, const char *version,
                               cJSON **refusal) {
    if (!runtime_bundle_is_intact(request->cache, id, version)) {
        return HINT_UNCHANGED;
    }
    spisor_lock *guard = runtime_updates_lock(request, refusal);
    runtime_host_references recorded = guard == NULL ? (runtime_host_references){NULL, NULL, NULL, NULL}
                                                     : runtime_read_host_references(request->state, request->host_id);
    int still_named = recorded.run != NULL && (names(recorded.run, id, version) || names(recorded.next, id, version));
    hint_switch outcome = HINT_UNCHANGED;
    if (still_named && runtime_bundle_is_sized(request->cache, id, version)) {
        outcome = runtime_hint_host_version(request->cache, id, version) ? HINT_SWITCHED : HINT_UNWRITTEN;
    }
    runtime_free_host_references(&recorded);
    spisor_unlock(guard);
    return outcome;
}

static int report_unwritten_hint(check_progress *progress, const char *name, const char *id, const char *version) {
    const char *parts[6] = {name, " is installed, but the boot hint of ", id, " could not be switched to ", version,
                            ", so every command still boots the version before it"};
    char *message = runtime_cache_text(parts, 6);
    int reported = message != NULL && add_refused(progress->refused, name, message, HINT_FIX);
    free(message);
    return reported;
}

/**
 * Installs one target, then switches its hint for a host the start keeps current.
 *
 * @implNote The install holds no lock: record_next already named the version in next under the lock, so
 * the sweep keeps it while it arrives, and a start or a registration never waits on a download.
 */
static int install_target(const runtime_updates_request *request, const check_plan *plan, const char *id,
                          const char *version, const char *name, check_progress *progress) {
    cJSON *refusal = NULL;
    cJSON *answer = runtime_cache_install_version(request->cache, id, runtime_updates_item(plan->catalog, id), version,
                                                  request->platform, &refusal);
    int done = answer != NULL ? cJSON_AddItemToArray(progress->fetched, cJSON_CreateString(name))
               : refusal != NULL ? add_refused(progress->refused, name, runtime_map_version(refusal, "message"),
                                               runtime_map_version(refusal, "fix"))
                                 : 0;
    cJSON_Delete(refusal);
    refusal = NULL;
    if (done && answer != NULL && is_kept_host(plan->hosts, id)) {
        hint_switch outcome = switch_hint(request, id, version, &refusal);
        done = refusal != NULL ? add_refused(progress->refused, name, runtime_map_version(refusal, "message"),
                                             runtime_map_version(refusal, "fix"))
               : outcome == HINT_UNWRITTEN ? report_unwritten_hint(progress, name, id, version)
                                           : 1;
        cJSON_Delete(refusal);
    }
    cJSON_Delete(answer);
    return done;
}

static int install_one(const runtime_updates_request *request, const check_plan *plan, const cJSON *target,
                       check_progress *progress) {
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
        done = install_target(request, plan, id, version, name, progress);
    }
    free(name);
    return done;
}

static int install_targets(const runtime_updates_request *request, const check_plan *plan, check_progress *progress) {
    int installed = 1;
    for (const cJSON *target = plan->targets->child; installed && target != NULL; target = target->next) {
        installed = install_one(request, plan, target, progress);
    }
    return installed;
}

/** Sweeps with the registry lock held, never through cache.sweep, which takes the lock itself. */
static cJSON *sweep(const runtime_updates_request *request, cJSON **refusal) {
    spisor_lock *guard = runtime_updates_lock(request, refusal);
    cJSON *removed = guard == NULL ? NULL : runtime_cache_sweep_versions(request->cache, request->state);
    spisor_unlock(guard);
    return removed;
}

static int plan_check(const runtime_updates_request *request, const char *now, check_plan *plan,
                      check_progress *progress, cJSON **refusal) {
    plan->sources = runtime_updates_sources(settings_of(request), refusal);
    int checked = plan->sources != NULL && check_sources(request, plan->sources, now, progress->checked, refusal);
    plan->catalog = checked ? runtime_updates_current_catalog(plan->sources, request->cache, refusal) : NULL;
    return plan->catalog != NULL && plan_targets(request, plan, refusal);
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
    cJSON_Delete(plan->rolled_back_from);
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
    check_plan plan = {NULL, NULL, NULL, NULL, NULL};
    check_progress progress = {cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateArray()};
    int done = progress.checked != NULL && progress.fetched != NULL && progress.refused != NULL &&
               plan_check(request, now, &plan, &progress, refusal) && record_next(request, &plan, refusal) &&
               install_targets(request, &plan, &progress);
    cJSON *removed = done ? sweep(request, refusal) : NULL;
    cJSON *answer = removed == NULL ? NULL : answer_of(&progress, removed);
    free_check(&plan, &progress);
    return answer;
}
