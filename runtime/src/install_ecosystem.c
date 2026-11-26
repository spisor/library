#include "install_ecosystem.h"
#include "allocation.h"

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

#define FOREBAY_LIBRARY_ID "forebay-library"
#define CATALOG_FIX "check the catalog path or url, then install again"
#define ECOSYSTEM_FIX                                                                                           \
    "send {\"sources\": [{\"kind\": \"local\", \"path\"} | {\"kind\": \"https\", \"url\"}], \"places\": "         \
    "{\"portable\", \"state\", \"cache\"}, \"environment\": {<name>: <value>}, \"choices\"?: {<application>: "  \
    "<method>}, \"defaults\"?: [<plugin id>], \"requested\"?: [<plugin id>], \"platform\"?: \"<os>-<arch>\"}"

static const char *const FOUNDATION_IDS[] = {RUNTIME_ECOSYSTEM_HOST_ID, RUNTIME_LIBRARY_ID, FOREBAY_LIBRARY_ID,
                                             RUNTIME_JAVASCRIPT_HOST_ID};

/** What one ecosystem install has settled so far, beside the run every step shares. */
typedef struct ecosystem {
    runtime_install_run run;
    runtime_install_settings settings;
    char *home;
    const cJSON *defaults;
    const cJSON *requested;
    cJSON *values;
    cJSON *running;
    runtime_install_ecosystem_taker take;
    void *taken;
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

/** @implNote An install is asked for, so it checks every source now rather than waiting out the update interval. */
static int check_source(ecosystem *install, const cJSON *source, const char *now) {
    cJSON *refusal = NULL;
    runtime_catalog_check_request request = {source, install->run.cache, now, 0};
    cJSON *checked = runtime_catalog_check(&request, &refusal);
    const char *kind = runtime_install_text(checked, "kind");
    int recorded = checked == NULL ? runtime_install_report_refusal(install->run.report, "catalog", refusal)
                   : kind != NULL && strcmp(kind, "failed") == 0
                       ? runtime_install_report_unavailable(install->run.report, "catalog",
                                                            runtime_install_text(checked, "detail"), CATALOG_FIX)
                       : 1;
    cJSON_Delete(checked);
    return recorded;
}

/**
 * Reads the catalog every source yields, each checked first; 0 with *refusal set when none yields one, so a source
 * that failed beside one that yields stays under `unavailable` and an install with no catalog writes nothing.
 */
static int read_catalog(ecosystem *install, const cJSON *sources, cJSON **refusal) {
    char now[RUNTIME_CATALOG_TIME_SIZE];
    size_t yielding = 0;
    if (!runtime_catalog_format_time(runtime_now_milliseconds(), now)) {
        return 0;
    }
    for (const cJSON *source = sources->child; source != NULL; source = source->next) {
        if (!check_source(install, source, now)) {
            return 0;
        }
    }
    install->run.catalog = runtime_catalog_current_counted(sources, install->run.cache, &yielding, refusal);
    if (install->run.catalog != NULL && yielding == 0) {
        cJSON_Delete(install->run.catalog);
        install->run.catalog = NULL;
        *refusal = runtime_refusal("no catalog source yields a catalog, so nothing can be installed", CATALOG_FIX);
    }
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
static int needs_enabling(const ecosystem *install, const char *id, const cJSON *mentioned) {
    int asked = runtime_install_holds(install->requested, id);
    return (asked || !runtime_install_holds(mentioned, id)) && !runtime_install_plugin_enabled(install->values, id);
}

/** Reports refusal, which it deletes, under each of ids; 0 when memory ran out. */
static int report_each_refused(ecosystem *install, const cJSON *ids, cJSON *refusal) {
    int reported = runtime_is_refusal(refusal);
    for (const cJSON *id = ids->child; reported && id != NULL; id = id->next) {
        reported = runtime_install_report_unavailable(install->run.report, id->valuestring,
                                                      runtime_install_text(refusal, "message"),
                                                      runtime_install_text(refusal, "fix"));
    }
    cJSON_Delete(refusal);
    return reported;
}

/**
 * @implNote One write enables every plugin this install enables, after all of them are installed: which plugins to
 * install is read from the settings, so a repeat after a failure must find them as this install found them or as it
 * left them, never with only some of the plugins enabled.
 */
static int enable_all(ecosystem *install, const cJSON *enabling) {
    if (cJSON_GetArraySize(enabling) == 0) {
        return 1;
    }
    cJSON *refusal = NULL;
    if (!runtime_install_enable_plugins(&install->settings, enabling, &refusal)) {
        return report_each_refused(install, enabling, refusal);
    }
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(install->run.report, "enabled");
    int reported = 1;
    for (const cJSON *id = enabling->child; reported && id != NULL; id = id->next) {
        reported = runtime_install_add_text(enabled, id->valuestring);
    }
    return reported;
}

static int install_plugins(ecosystem *install) {
    cJSON *targets = plugin_targets(install);
    cJSON *mentioned = targets == NULL ? NULL : mentioned_plugins(install->values);
    cJSON *enabling = mentioned == NULL ? NULL : cJSON_CreateArray();
    int installed = enabling != NULL;
    for (const cJSON *target = installed ? targets->child : NULL; installed && target != NULL; target = target->next) {
        const char *id = runtime_install_text(target, "id");
        installed = install_referenced(install, target, "plugin") &&
                    (!cJSON_HasObjectItem(install->running, id) || !needs_enabling(install, id, mentioned) ||
                     runtime_install_add_text(enabling, id));
    }
    installed = installed && enable_all(install, enabling);
    cJSON_Delete(enabling);
    cJSON_Delete(mentioned);
    cJSON_Delete(targets);
    return installed;
}

static int record_sources(ecosystem *install, const cJSON *sources) {
    cJSON *refusal = NULL;
    return runtime_install_record_sources(&install->settings, sources, &refusal) ||
           (refusal != NULL && runtime_install_report_refusal(install->run.report, "catalog", refusal));
}

/** The running versions of the libraries, as an object of library id to version; NULL when memory runs out. */
static cJSON *hint_libraries(const ecosystem *install, const char *const *libraries, size_t count) {
    cJSON *versions = cJSON_CreateObject();
    int built = versions != NULL;
    for (size_t index = 0; built && index < count; index++) {
        built = runtime_install_copy_into(versions, libraries[index],
                                          cJSON_GetObjectItemCaseSensitive(install->running, libraries[index]));
    }
    if (!built) {
        cJSON_Delete(versions);
        return NULL;
    }
    return versions;
}

/** Records the libraries and host version the next start of host_id boots, when that host is installed. */
static int write_hint(ecosystem *install, const char *host_id, const char *const *libraries, size_t count) {
    if (!cJSON_HasObjectItem(install->running, host_id)) {
        return 1;
    }
    cJSON *refusal = NULL;
    cJSON *versions = hint_libraries(install, libraries, count);
    runtime_boot_hint_request request = {install->run.cache,
                                         host_id,
                                         versions,
                                         runtime_install_text(install->running, host_id),
                                         install->run.state,
                                         install->run.platform};
    int written = versions != NULL && runtime_boot_write(&request, &refusal);
    cJSON_Delete(versions);
    return written || (refusal != NULL && runtime_install_report_refusal(install->run.report, host_id, refusal));
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
        const cJSON *entry = cJSON_GetObjectItemCaseSensitive(row, "entry");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "runsOnJavascriptHost")) &&
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "written"))) {
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
 * @implNote The JavaScript host and forebay-library come only with something that runs on them,
 * and Node only with something this install would start on it: an entry running in the application's
 * own process runs on the application's own runtime. forebay-library comes with every JavaScript host because any plugin it runs
 * may consume the services the host offers from it. Node is acquired after the host's hint is written, since the
 * host's host.json, which that hint names, carries the Node pin.
 */
static int install_javascript(ecosystem *install, const cJSON *background) {
    int starts = runs_javascript(install, background);
    if (!starts && !writes_in_process_entry(install)) {
        return 1;
    }
    static const char *const host_ids[1] = {RUNTIME_JAVASCRIPT_HOST_ID};
    static const char *const library_ids[1] = {FOREBAY_LIBRARY_ID};
    const char *libraries[2] = {RUNTIME_LIBRARY_ID, FOREBAY_LIBRARY_ID};
    return install_pinned(install, library_ids, 1, "library") && install_pinned(install, host_ids, 1, "host") &&
           write_hint(install, RUNTIME_JAVASCRIPT_HOST_ID, libraries, 2) && (!starts || acquire_javascript(install));
}

static int record_run(ecosystem *install) {
    cJSON *refusal = NULL;
    return runtime_install_replace_run(install->run.state, RUNTIME_ECOSYSTEM_HOST_ID, install->running, &refusal) ||
           (refusal != NULL && runtime_install_report_refusal(install->run.report, RUNTIME_ECOSYSTEM_HOST_ID, refusal));
}

static int take_report(ecosystem *install) {
    return install->take == NULL || install->take(install->run.report, install->taken) == SPISOR_OK;
}

static int unavailable_count(const ecosystem *install) {
    return cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(install->run.report, "unavailable"));
}

/** Records the run with the report taken first, and taken again when the record's refusal adds to it. */
static int record_run_taken(ecosystem *install) {
    int before = unavailable_count(install);
    return take_report(install) && record_run(install) && (unavailable_count(install) == before || take_report(install));
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
    int done = background != NULL && install_javascript(install, background) && record_run_taken(install);
    cJSON_Delete(background);
    return done;
}

static int read_request(const runtime_install_ecosystem_request *request, ecosystem *install) {
    install->run.places = request->places;
    install->run.portable = runtime_install_place(request->places, "portable");
    install->run.state = runtime_install_place(request->places, "state");
    install->run.cache = runtime_install_place(request->places, "cache");
    install->run.environment = request->environment;
    install->run.choices = request->choices;
    install->run.platform = request->platform == NULL ? runtime_platform_key() : request->platform;
    install->defaults = request->defaults;
    install->requested = request->requested;
    return install->run.portable != NULL && install->run.state != NULL && install->run.cache != NULL &&
           cJSON_IsObject(install->run.environment) && is_choice_map(install->run.choices) &&
           (install->defaults == NULL || is_text_array(install->defaults)) &&
           (install->requested == NULL || is_text_array(install->requested)) && install->run.platform != NULL &&
           runtime_is_platform_key(install->run.platform);
}

/**
 * Whether sources is an array of catalog sources as catalog.sources reads them; 0 with *refusal set otherwise, or
 * left NULL when memory ran out reading them.
 */
static int are_sources(const cJSON *sources, cJSON **refusal) {
    runtime_catalog_location *read = NULL;
    size_t count = 0;
    char *problem = NULL;
    int is_array = cJSON_IsArray(sources);
    int usable = is_array && runtime_catalog_sources_of(sources, &read, &count, &problem);
    if (!usable && (!is_array || problem != NULL)) {
        const char *parts[2] = {"the sources hold an entry that is no catalog source: ",
                                problem == NULL ? "they are not an array" : problem};
        *refusal = runtime_cache_refusal(parts, 2, ECOSYSTEM_FIX);
    }
    spisor_memory_release(problem);
    spisor_memory_release(read);
    return usable;
}

cJSON *runtime_install_ecosystem(const runtime_install_ecosystem_request *request,
                                 runtime_install_ecosystem_taker take, void *taken, cJSON **refusal) {
    *refusal = NULL;
    if (!are_sources(request->sources, refusal)) {
        return NULL;
    }
    ecosystem install;
    memset(&install, 0, sizeof install);
    install.take = take;
    install.taken = taken;
    if (!read_request(request, &install)) {
        *refusal = runtime_refusal("the call carries no usable sources, places, environment or choices", ECOSYSTEM_FIX);
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
               install_all(&install, request->sources, refusal);
    cJSON *report = done ? install.run.report : NULL;
    if (!done) {
        cJSON_Delete(install.run.report);
    }
    cJSON_Delete(install.running);
    cJSON_Delete(install.run.required_plugins);
    cJSON_Delete(install.values);
    cJSON_Delete(install.run.catalog);
    spisor_memory_release(install.home);
    return report;
}

