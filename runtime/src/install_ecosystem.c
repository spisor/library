#include "install_ecosystem.h"

#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "cache_refusal.h"
#include "catalog.h"
#include "catalog_source.h"
#include "catalog_time.h"
#include "file_times.h"
#include "install_entries.h"
#include "install_hosts.h"
#include "install_references.h"
#include "install_report.h"
#include "install_run.h"
#include "install_runtime.h"
#include "install_settings.h"
#include "install_support.h"
#include "json_service.h"
#include "platform_key.h"
#include "plugin_choices.h"

#define RUNTIME_LIBRARY_ID "runtime"
#define KOFFI_LIBRARY_ID "koffi"
#define APPS_LIBRARY_ID "apps"
#define CATALOG_FIX "check the catalog path or url, then install again"
#define ECOSYSTEM_FIX                                                                                           \
    "send {\"sources\": [{\"kind\": \"local\", \"path\"} | {\"kind\": \"https\", \"url\"}], \"places\": "         \
    "{\"portable\", \"state\", \"cache\"}, \"environment\": {<name>: <value>}, \"choices\"?: {<application>: "  \
    "<method>}, \"defaults\"?: [<plugin id>], \"requested\"?: [<plugin id>], \"platform\"?: \"<os>-<arch>\"}"

static const char *const FOUNDATION_IDS[] = {RUNTIME_ECOSYSTEM_HOST_ID, RUNTIME_LIBRARY_ID, KOFFI_LIBRARY_ID,
                                             APPS_LIBRARY_ID, RUNTIME_JAVASCRIPT_HOST_ID};

/** What one ecosystem install has settled so far, beside the run every step shares. */
typedef struct ecosystem {
    runtime_install_run run;
    runtime_install_settings settings;
    char *home;
    const cJSON *defaults;
    const cJSON *requested;
    cJSON *values;
    cJSON *running;
} ecosystem;

static int is_foundation(const char *id) {
    for (size_t index = 0; index < sizeof FOUNDATION_IDS / sizeof FOUNDATION_IDS[0]; index++) {
        if (strcmp(FOUNDATION_IDS[index], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_text_array(const cJSON *value) {
    if (!cJSON_IsArray(value)) {
        return 0;
    }
    for (const cJSON *entry = value->child; entry != NULL; entry = entry->next) {
        if (!cJSON_IsString(entry)) {
            return 0;
        }
    }
    return 1;
}

static int is_choice_map(const cJSON *value) {
    if (value == NULL) {
        return 1;
    }
    if (!cJSON_IsObject(value)) {
        return 0;
    }
    for (const cJSON *choice = value->child; choice != NULL; choice = choice->next) {
        if (!cJSON_IsString(choice) || choice->valuestring[0] == 0) {
            return 0;
        }
    }
    return 1;
}

static cJSON *check_call(const ecosystem *install, const cJSON *source, const char *now) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !runtime_install_copy_into(call, "source", source) ||
        cJSON_AddStringToObject(call, "cache", install->run.cache) == NULL || cJSON_AddStringToObject(call, "now", now) == NULL ||
        cJSON_AddNumberToObject(call, "intervalHours", 0) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** @implNote An install is asked for, so it checks every source now rather than waiting out the update interval. */
static int check_source(ecosystem *install, const cJSON *source, const char *now) {
    cJSON *refusal = NULL;
    cJSON *checked = runtime_call_or_refuse(runtime_catalog_check, check_call(install, source, now), &refusal);
    const char *kind = runtime_install_text(checked, "kind");
    int recorded = checked == NULL ? runtime_install_report_refusal(install->run.report, "catalog", refusal)
                   : kind != NULL && strcmp(kind, "failed") == 0
                       ? runtime_install_report_unavailable(install->run.report, "catalog",
                                                            runtime_install_text(checked, "detail"), CATALOG_FIX)
                       : 1;
    cJSON_Delete(checked);
    return recorded;
}

static cJSON *current_catalog(const ecosystem *install, const cJSON *sources, cJSON **refusal) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !runtime_install_copy_into(call, "sources", sources) ||
        cJSON_AddStringToObject(call, "cache", install->run.cache) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    cJSON *answer = runtime_call_or_refuse(runtime_catalog_current, call, refusal);
    cJSON *catalog = cJSON_DetachItemFromObjectCaseSensitive(answer, "catalog");
    cJSON_Delete(answer);
    return catalog;
}

static int read_catalog(ecosystem *install, const cJSON *sources, cJSON **refusal) {
    char now[RUNTIME_CATALOG_TIME_SIZE];
    if (!runtime_catalog_format_time(runtime_now_milliseconds(), now)) {
        return 0;
    }
    for (const cJSON *source = sources->child; source != NULL; source = source->next) {
        if (!check_source(install, source, now)) {
            return 0;
        }
    }
    install->run.catalog = current_catalog(install, sources, refusal);
    return install->run.catalog != NULL;
}

static int note_running(ecosystem *install, const char *id, const char *version) {
    cJSON_DeleteItemFromObjectCaseSensitive(install->running, id);
    return cJSON_AddStringToObject(install->running, id, version) != NULL;
}

/** Installs target {id, pin?} of kind, referenced for the ecosystem host first; 0 only when memory runs out. */
static int install_referenced(ecosystem *install, const cJSON *target, const char *kind) {
    const char *id = runtime_install_text(target, "id");
    const char *version = runtime_install_target_version(install->run.catalog, target, kind);
    cJSON *versions = version == NULL ? NULL : cJSON_CreateObject();
    cJSON *refusal = NULL;
    int referenced = versions != NULL && cJSON_AddStringToObject(versions, id, version) != NULL &&
                     runtime_install_reference_versions(install->run.state, RUNTIME_ECOSYSTEM_HOST_ID, versions, &refusal);
    cJSON_Delete(versions);
    if (version != NULL && !referenced) {
        return runtime_install_report_refusal(install->run.report, id, refusal);
    }
    int out_of_memory = 0;
    const char *installed = runtime_install_item(install->run.catalog, target, kind, install->run.cache, install->run.report,
                                                 &out_of_memory);
    return !out_of_memory && (installed == NULL || note_running(install, id, installed));
}

static int install_pinned(ecosystem *install, const char *const *ids, size_t count, const char *kind) {
    cJSON *names = cJSON_CreateArray();
    for (size_t index = 0; names != NULL && index < count; index++) {
        if (!runtime_install_add_text(names, ids[index])) {
            cJSON_Delete(names);
            names = NULL;
        }
    }
    cJSON *targets = names == NULL ? NULL : runtime_pinned_candidates(names, install->values);
    int installed = targets != NULL;
    for (const cJSON *target = installed ? targets->child : NULL; installed && target != NULL; target = target->next) {
        installed = install_referenced(install, target, kind);
    }
    cJSON_Delete(targets);
    cJSON_Delete(names);
    return installed;
}

/** The plugin ids the settings mention under plugins, a pin on the host or a library not making one. */
static cJSON *mentioned_plugins(const cJSON *values) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(values, "plugins");
    cJSON *mentioned = cJSON_CreateArray();
    for (const cJSON *member = mentioned != NULL && cJSON_IsObject(plugins) ? plugins->child : NULL; member != NULL;
         member = member->next) {
        if (!is_foundation(member->string) && !runtime_install_add_text(mentioned, member->string)) {
            cJSON_Delete(mentioned);
            return NULL;
        }
    }
    return mentioned;
}

static int append_unnamed(cJSON *targets, const cJSON *asked, const cJSON *values) {
    cJSON *unnamed = cJSON_CreateArray();
    for (const cJSON *id = unnamed == NULL || asked == NULL ? NULL : asked->child; id != NULL; id = id->next) {
        int named = runtime_install_holds(unnamed, id->valuestring);
        for (const cJSON *target = targets->child; !named && target != NULL; target = target->next) {
            named = strcmp(runtime_install_text(target, "id"), id->valuestring) == 0;
        }
        if (!named && !runtime_install_add_text(unnamed, id->valuestring)) {
            cJSON_Delete(unnamed);
            return 0;
        }
    }
    cJSON *pinned = unnamed == NULL ? NULL : runtime_pinned_candidates(unnamed, values);
    while (pinned != NULL && pinned->child != NULL) {
        cJSON_AddItemToArray(targets, cJSON_DetachItemViaPointer(pinned, pinned->child));
    }
    int appended = pinned != NULL;
    cJSON_Delete(pinned);
    cJSON_Delete(unnamed);
    return appended;
}

/** The enabled plugins, or defaults when the settings name none, then each requested plugin not named yet. */
static cJSON *plugin_targets(const ecosystem *install) {
    cJSON *mentioned = mentioned_plugins(install->values);
    cJSON *targets = mentioned == NULL ? NULL
                     : cJSON_GetArraySize(mentioned) == 0 ? cJSON_CreateArray()
                                                          : runtime_plugin_candidates(install->values, NULL);
    if (targets != NULL && cJSON_GetArraySize(mentioned) == 0 && !append_unnamed(targets, install->defaults, install->values)) {
        cJSON_Delete(targets);
        targets = NULL;
    }
    if (targets != NULL && !append_unnamed(targets, install->requested, install->values)) {
        cJSON_Delete(targets);
        targets = NULL;
    }
    cJSON_Delete(mentioned);
    return targets;
}

/** @implNote Only an id the settings never mention is enabled unasked, so a plugin the user turned off stays off. */
static int enable_choice(ecosystem *install, const char *id, const cJSON *mentioned) {
    int asked = runtime_install_holds(install->requested, id);
    if ((!asked && runtime_install_holds(mentioned, id)) || runtime_install_plugin_enabled(install->values, id)) {
        return 1;
    }
    cJSON *refusal = NULL;
    int enabled = runtime_install_enable_plugin(&install->settings, id, "portable", &refusal);
    if (!enabled) {
        return refusal != NULL && runtime_install_report_refusal(install->run.report, id, refusal);
    }
    return runtime_install_add_text(cJSON_GetObjectItemCaseSensitive(install->run.report, "enabled"), id);
}

static int install_plugins(ecosystem *install) {
    cJSON *targets = plugin_targets(install);
    cJSON *mentioned = targets == NULL ? NULL : mentioned_plugins(install->values);
    int installed = mentioned != NULL;
    for (const cJSON *target = installed ? targets->child : NULL; installed && target != NULL; target = target->next) {
        const char *id = runtime_install_text(target, "id");
        installed = install_referenced(install, target, "plugin") &&
                    (!cJSON_HasObjectItem(install->running, id) || enable_choice(install, id, mentioned));
    }
    cJSON_Delete(mentioned);
    cJSON_Delete(targets);
    return installed;
}

static int record_sources(ecosystem *install, const cJSON *sources) {
    cJSON *refusal = NULL;
    return runtime_install_record_sources(&install->settings, sources, &refusal) ||
           (refusal != NULL && runtime_install_report_refusal(install->run.report, "catalog", refusal));
}

static cJSON *hint_call(const ecosystem *install, const char *host_id, const char *const *libraries, size_t count) {
    cJSON *call = cJSON_CreateObject();
    cJSON *versions = call == NULL ? NULL : cJSON_AddObjectToObject(call, "libraries");
    int built = versions != NULL && cJSON_AddStringToObject(call, "cache", install->run.cache) != NULL &&
                cJSON_AddStringToObject(call, "hostId", host_id) != NULL &&
                cJSON_AddStringToObject(call, "state", install->run.state) != NULL &&
                runtime_install_copy_into(call, "host", cJSON_GetObjectItemCaseSensitive(install->running, host_id));
    for (size_t index = 0; built && index < count; index++) {
        built = runtime_install_copy_into(versions, libraries[index], cJSON_GetObjectItemCaseSensitive(install->running, libraries[index]));
    }
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** Records the libraries and host version the next start of host_id boots, when that host is installed. */
static int write_hint(ecosystem *install, const char *host_id, const char *const *libraries, size_t count) {
    if (!cJSON_HasObjectItem(install->running, host_id)) {
        return 1;
    }
    cJSON *refusal = NULL;
    cJSON *hint = runtime_call_or_refuse(runtime_boot_write, hint_call(install, host_id, libraries, count), &refusal);
    cJSON_Delete(hint);
    return hint != NULL || (refusal != NULL && runtime_install_report_refusal(install->run.report, host_id, refusal));
}

static int register_ecosystem_host(ecosystem *install) {
    const char *version = runtime_install_text(install->running, RUNTIME_ECOSYSTEM_HOST_ID);
    cJSON *refusal = NULL;
    const char *libraries[1] = {RUNTIME_LIBRARY_ID};
    if (version != NULL && !runtime_install_register_own_host(install->run.places, RUNTIME_ECOSYSTEM_HOST_ID, version, NULL, &refusal) &&
        !(refusal != NULL && runtime_install_report_refusal(install->run.report, RUNTIME_ECOSYSTEM_HOST_ID, refusal))) {
        return 0;
    }
    return write_hint(install, RUNTIME_ECOSYSTEM_HOST_ID, libraries, 1);
}

static const char *item_member(const ecosystem *install, const char *id, const char *member) {
    return runtime_install_text(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(install->run.catalog, "items"), id),
                                member);
}

static int is_text(const char *text, const char *expected) {
    return text != NULL && strcmp(text, expected) == 0;
}

/** Whether a plugin or a background host installed now runs on the JavaScript runtime this install would start it with. */
static int runs_javascript(const ecosystem *install, const cJSON *background) {
    for (const cJSON *version = install->running->child; version != NULL; version = version->next) {
        if (is_text(item_member(install, version->string, "kind"), "plugin") &&
            is_text(item_member(install, version->string, "runtime"), "javascript")) {
            return 1;
        }
    }
    for (const cJSON *id = background->child; id != NULL; id = id->next) {
        if (is_text(item_member(install, id->valuestring, "runtime"), "javascript")) {
            return 1;
        }
    }
    for (const cJSON *plugin = install->run.required_plugins->child; plugin != NULL; plugin = plugin->next) {
        if (is_text(item_member(install, plugin->string, "runtime"), "javascript")) {
            return 1;
        }
    }
    return 0;
}

static int writes_in_process_entry(const ecosystem *install) {
    for (const cJSON *row = cJSON_GetObjectItemCaseSensitive(install->run.report, "loaders")->child; row != NULL; row = row->next) {
        if (runtime_entry_needs_javascript_host(runtime_install_text(row, "application")) &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(row, "entry"), "written"))) {
            return 1;
        }
    }
    return 0;
}

static int acquire_javascript(ecosystem *install) {
    cJSON *refusal = NULL;
    cJSON *acquired = runtime_acquire_javascript(install->run.cache, install->run.platform, install->run.environment, &refusal);
    if (acquired == NULL) {
        return refusal != NULL && runtime_install_report_refusal(install->run.report, "node", refusal);
    }
    return cJSON_AddItemToObject(install->run.report, "javascript", acquired);
}

/**
 * @implNote The JavaScript host, koffi and the apps library come only with something that runs on them,
 * and Node only with something this install would start on it: OpenCode's in-process entry runs in
 * OpenCode's own runtime. The apps library comes with every JavaScript host because any plugin it runs
 * may consume the services the host offers from it.
 */
static int install_javascript(ecosystem *install, const cJSON *background) {
    int starts = runs_javascript(install, background);
    if (!starts && !writes_in_process_entry(install)) {
        return 1;
    }
    static const char *const host_ids[1] = {RUNTIME_JAVASCRIPT_HOST_ID};
    static const char *const library_ids[2] = {KOFFI_LIBRARY_ID, APPS_LIBRARY_ID};
    const char *libraries[3] = {RUNTIME_LIBRARY_ID, KOFFI_LIBRARY_ID, APPS_LIBRARY_ID};
    return install_pinned(install, library_ids, 2, "library") && install_pinned(install, host_ids, 1, "host") &&
           write_hint(install, RUNTIME_JAVASCRIPT_HOST_ID, libraries, 3) && (!starts || acquire_javascript(install));
}

static int record_run(ecosystem *install) {
    cJSON *refusal = NULL;
    return runtime_install_replace_run(install->run.state, RUNTIME_ECOSYSTEM_HOST_ID, install->running, &refusal) ||
           (refusal != NULL && runtime_install_report_refusal(install->run.report, RUNTIME_ECOSYSTEM_HOST_ID, refusal));
}

static int install_all(ecosystem *install, const cJSON *sources, cJSON **refusal) {
    static const char *const host_ids[1] = {RUNTIME_ECOSYSTEM_HOST_ID};
    static const char *const library_ids[1] = {RUNTIME_LIBRARY_ID};
    if (!read_catalog(install, sources, refusal) || !runtime_install_resolve_applications(&install->run, refusal)) {
        return 0;
    }
    install->values = runtime_install_settings_values(&install->settings, 1, 1, refusal);
    if (install->values == NULL || !install_pinned(install, host_ids, 1, "host") ||
        !install_pinned(install, library_ids, 1, "library") || !install_plugins(install) ||
        !record_sources(install, sources) || !register_ecosystem_host(install)) {
        return 0;
    }
    cJSON *background = runtime_install_loaders(&install->run);
    int done = background != NULL && install_javascript(install, background) && record_run(install);
    cJSON_Delete(background);
    return done;
}

static int read_argument(const cJSON *argument, ecosystem *install) {
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    install->run.places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    install->run.portable = runtime_install_place(argument, "portable");
    install->run.state = runtime_install_place(argument, "state");
    install->run.cache = runtime_install_place(argument, "cache");
    install->run.environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    install->run.choices = cJSON_GetObjectItemCaseSensitive(argument, "choices");
    install->run.platform = platform == NULL ? runtime_platform_key() : cJSON_GetStringValue(platform);
    install->defaults = cJSON_GetObjectItemCaseSensitive(argument, "defaults");
    install->requested = cJSON_GetObjectItemCaseSensitive(argument, "requested");
    return install->run.portable != NULL && install->run.state != NULL && install->run.cache != NULL &&
           cJSON_IsObject(install->run.environment) && is_choice_map(install->run.choices) &&
           (install->defaults == NULL || is_text_array(install->defaults)) &&
           (install->requested == NULL || is_text_array(install->requested)) && install->run.platform != NULL &&
           runtime_is_platform_key(install->run.platform);
}

/** Whether sources is an array of catalog sources as catalog.sources reads them; 0 with *refusal set otherwise. */
static int are_sources(const cJSON *sources, cJSON **refusal) {
    runtime_catalog_source *read = NULL;
    size_t count = 0;
    char *problem = NULL;
    int usable = cJSON_IsArray(sources) && runtime_catalog_sources_of(sources, &read, &count, &problem);
    if (!usable) {
        const char *parts[2] = {"the sources hold an entry that is no catalog source: ",
                                problem == NULL ? "they are not an array" : problem};
        *refusal = runtime_cache_refusal(parts, 2, ECOSYSTEM_FIX);
    }
    free(problem);
    free(read);
    return usable;
}

static cJSON *ecosystem_for(const cJSON *argument, cJSON **refusal) {
    if (!are_sources(cJSON_GetObjectItemCaseSensitive(argument, "sources"), refusal)) {
        return NULL;
    }
    ecosystem install;
    memset(&install, 0, sizeof install);
    if (!read_argument(argument, &install)) {
        *refusal = runtime_refusal("the argument carries no usable sources, places, environment or choices", ECOSYSTEM_FIX);
        return NULL;
    }
    install.home = runtime_install_own_home(install.run.state, RUNTIME_ECOSYSTEM_HOST_ID);
    install.settings.home = install.home;
    install.settings.portable = install.run.portable;
    install.settings.state = install.run.state;
    install.run.report = runtime_install_new_report();
    install.running = cJSON_CreateObject();
    install.run.required_plugins = cJSON_CreateObject();
    int done = install.home != NULL && install.run.report != NULL && install.running != NULL &&
               install.run.required_plugins != NULL &&
               install_all(&install, cJSON_GetObjectItemCaseSensitive(argument, "sources"), refusal);
    cJSON *report = done ? install.run.report : NULL;
    if (!done) {
        cJSON_Delete(install.run.report);
    }
    cJSON_Delete(install.running);
    cJSON_Delete(install.run.required_plugins);
    cJSON_Delete(install.values);
    cJSON_Delete(install.run.catalog);
    free(install.home);
    return report;
}

RUNTIME_EXPORT int32_t runtime_install_ecosystem(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, ecosystem_for);
}
