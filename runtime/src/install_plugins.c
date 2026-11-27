#include "install_plugins.h"
#include "allocation.h"

#include <string.h>

#include "boot_hint.h"
#include "cache_refusal.h"
#include "host_references.h"
#include "hosts_layers.h"
#include "install_entries.h"
#include "install_hosts.h"
#include "install_items.h"
#include "install_report.h"
#include "install_settings.h"
#include "install_support.h"
#include "json_service.h"
#include "platform_key.h"
#include "plugin_choices.h"

#define UNLISTED_FIX "add a catalog source listing it to spisor-host's settings, then install it again"
#define UNHINTED_FIX "install the ecosystem with spisor-host install, then install the plugin again"

/** One install of plugins for a host: the run every step shares and what the host's layers stand on. */
typedef struct plugins_install {
    runtime_install_run run;
    runtime_loader_layers layers;
    const char *host_id;
    const cJSON *plugins;
    cJSON *own_values;
    cJSON *installed;
} plugins_install;

static int is_text(const char *text, const char *expected) {
    return text != NULL && strcmp(text, expected) == 0;
}

static const cJSON *catalog_item(const cJSON *catalog, const char *id) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), id);
}

static cJSON *unlisted_refusal(const char *id) {
    const char *parts[3] = {"no catalog source lists ", id, " as a plugin, so it cannot be installed"};
    return runtime_cache_refusal(parts, 3, UNLISTED_FIX);
}

/**
 * Whether the catalog lists every plugin asked for as a plugin, in a language it lists a host for; 0 with *refusal
 * naming the first it does not, or with none when memory runs out.
 */
static int are_installable(const cJSON *catalog, const cJSON *plugins, cJSON **refusal) {
    for (const cJSON *id = plugins->child; id != NULL; id = id->next) {
        const char *language = runtime_plugin_language(catalog, id->valuestring);
        int out_of_memory = 0;
        if (!is_text(runtime_install_text(catalog_item(catalog, id->valuestring), "kind"), "plugin")) {
            *refusal = unlisted_refusal(id->valuestring);
            return 0;
        }
        if (language != NULL && runtime_language_host_item(catalog, language, &out_of_memory) == NULL) {
            *refusal = out_of_memory ? NULL : runtime_unhosted_language_refusal(language);
            return 0;
        }
    }
    return 1;
}

/** Installs each plugin at its pin or latest, referenced for the host, into install->installed; 0 on OOM. */
static int install_each(plugins_install *install) {
    cJSON *targets = runtime_pinned_candidates(install->plugins, install->layers.settings);
    runtime_install_items items = {&install->run, install->layers.settings, install->installed};
    int installed = targets != NULL;
    for (const cJSON *target = installed ? targets->child : NULL; installed && target != NULL; target = target->next) {
        installed = runtime_install_referenced(&items, install->host_id, target, "plugin");
    }
    cJSON_Delete(targets);
    return installed;
}

/** Reports refusal, which it deletes, under each of ids; 0 when memory ran out. */
static int report_each_refused(plugins_install *install, const cJSON *ids, cJSON *refusal) {
    int reported = runtime_is_refusal(refusal);
    for (const cJSON *id = ids->child; reported && id != NULL; id = id->next) {
        reported = runtime_install_report_unavailable(install->run.report, id->valuestring,
                                                      runtime_install_text(refusal, "message"),
                                                      runtime_install_text(refusal, "fix"));
    }
    cJSON_Delete(refusal);
    return reported;
}

/** The plugins installed now that the host's settings do not enable yet, as an array of ids; NULL on OOM. */
static cJSON *not_yet_enabled(const plugins_install *install) {
    cJSON *ids = cJSON_CreateArray();
    for (const cJSON *version = ids == NULL ? NULL : install->installed->child; version != NULL;
         version = version->next) {
        if (!runtime_install_plugin_enabled(install->layers.settings, version->string) &&
            !runtime_install_add_text(ids, version->string)) {
            cJSON_Delete(ids);
            return NULL;
        }
    }
    return ids;
}

/**
 * Enables every plugin installed now in one write to the host's application layer; 0 only when memory runs out.
 *
 * @implNote The application layer is the host's own: the portable layer is read by every host, so enabling a row's
 * plugin there would enable it for every loader.
 */
static int enable_installed(plugins_install *install) {
    runtime_install_settings settings = {runtime_install_text(install->layers.registration, "home"),
                                         install->run.portable, install->run.state};
    cJSON *enabling = not_yet_enabled(install);
    cJSON *refusal = NULL;
    int done = enabling != NULL;
    if (done && cJSON_GetArraySize(enabling) > 0) {
        if (!runtime_install_enable_plugins(&settings, enabling, "application", &refusal)) {
            done = report_each_refused(install, enabling, refusal);
        } else {
            cJSON *enabled = cJSON_GetObjectItemCaseSensitive(install->run.report, "enabled");
            for (const cJSON *id = enabling->child; done && id != NULL; id = id->next) {
                done = runtime_install_add_text(enabled, id->valuestring);
            }
        }
    }
    cJSON_Delete(enabling);
    return done;
}

/** Whether language's host item has a hint naming the version it runs; -1 when memory runs out. */
static int is_hinted(const char *cache, const char *language) {
    char *host_id = runtime_language_host_id(language);
    int out_of_memory = host_id == NULL;
    cJSON *hint = host_id == NULL ? NULL : runtime_read_boot_hint(cache, host_id, &out_of_memory);
    int hinted = runtime_install_text(hint, "host") != NULL;
    cJSON_Delete(hint);
    spisor_memory_release(host_id);
    return out_of_memory ? -1 : hinted;
}

/** The languages of the plugins installed now whose host item no hint names yet, as an array; NULL on OOM. */
static cJSON *unhinted_languages(const plugins_install *install) {
    cJSON *languages = cJSON_CreateArray();
    int built = languages != NULL;
    for (const cJSON *version = built ? install->installed->child : NULL; built && version != NULL;
         version = version->next) {
        const char *language = runtime_plugin_language(install->run.catalog, version->string);
        int hinted = language == NULL || runtime_install_holds(languages, language) ? 1
                                                                                      : is_hinted(install->run.cache,
                                                                                                  language);
        built = hinted == 1 || (hinted == 0 && runtime_install_add_text(languages, language));
    }
    if (!built) {
        cJSON_Delete(languages);
        return NULL;
    }
    return languages;
}

/** Reports language's host unavailable for want of a runtime spisor-host runs; 0 when memory runs out. */
static int report_no_runtime(plugins_install *install, const char *language) {
    char *host_id = runtime_language_host_id(language);
    const char *parts[3] = {"spisor-host runs no runtime library, so ", host_id, " cannot be hinted to run on one"};
    char *message = host_id == NULL ? NULL : runtime_cache_text(parts, 3);
    int reported = message != NULL &&
                   runtime_install_report_unavailable(install->run.report, host_id, message, UNHINTED_FIX);
    spisor_memory_release(message);
    spisor_memory_release(host_id);
    return reported;
}

static int runs_on_javascript(const plugins_install *install, const char *language) {
    char *host_id = runtime_language_host_id(language);
    int javascript = host_id != NULL &&
                     is_text(runtime_install_text(catalog_item(install->run.catalog, host_id), "runtime"),
                             RUNTIME_LAYER_JAVASCRIPT);
    spisor_memory_release(host_id);
    return javascript;
}

/**
 * Installs and hints each language host the plugins installed now need and no hint names yet, on what spisor-host
 * runs, then the pinned Node when one of them runs on JavaScript; 0 only when memory runs out.
 */
static int install_language_hosts(plugins_install *install) {
    static const char *const library_ids[1] = {RUNTIME_FOREBAY_LIBRARY_ID};
    cJSON *languages = unhinted_languages(install);
    runtime_host_references references = runtime_read_host_references(install->run.state, RUNTIME_ECOSYSTEM_HOST_ID);
    runtime_install_items items = {&install->run, install->own_values, references.run};
    int javascript = 0;
    int done = languages != NULL && references.run != NULL;
    for (const cJSON *language = done ? languages->child : NULL; done && language != NULL; language = language->next) {
        if (!cJSON_HasObjectItem(references.run, RUNTIME_LIBRARY_ID)) {
            done = report_no_runtime(install, language->valuestring);
            continue;
        }
        done = (cJSON_HasObjectItem(references.run, RUNTIME_FOREBAY_LIBRARY_ID) ||
                runtime_install_pinned(&items, library_ids, 1, "library")) &&
               runtime_install_language_host(&items, language->valuestring);
        javascript = javascript || runs_on_javascript(install, language->valuestring);
    }
    runtime_free_host_references(&references);
    cJSON_Delete(languages);
    return done && (!javascript || runtime_install_acquire_javascript(&install->run));
}

static int install_all(plugins_install *install) {
    return install_each(install) && enable_installed(install) && install_language_hosts(install);
}

/** Reads what the install stands on, writing nothing; 0 with *refusal set, or with none when memory runs out. */
static int read_layers(plugins_install *install, cJSON **refusal) {
    char *own_home = runtime_install_own_home(install->run.state, RUNTIME_ECOSYSTEM_HOST_ID);
    runtime_install_settings own = {own_home, install->run.portable, install->run.state};
    int read = own_home != NULL && runtime_read_loader_layers(install->run.places, install->host_id, &install->layers,
                                                              refusal);
    install->run.catalog = install->layers.catalog;
    read = read && are_installable(install->run.catalog, install->plugins, refusal);
    install->own_values = read ? runtime_install_settings_values(&own, 1, 1, refusal) : NULL;
    spisor_memory_release(own_home);
    return install->own_values != NULL;
}

static int is_usable(const runtime_install_plugins_request *request, plugins_install *install) {
    int plugins = cJSON_IsArray(request->plugins) && cJSON_GetArraySize(request->plugins) > 0;
    for (const cJSON *id = plugins ? request->plugins->child : NULL; id != NULL; id = id->next) {
        plugins = plugins && cJSON_IsString(id) && id->valuestring[0] != 0;
    }
    install->run.places = request->places;
    install->run.portable = runtime_install_place(request->places, "portable");
    install->run.state = runtime_install_place(request->places, "state");
    install->run.cache = runtime_install_place(request->places, "cache");
    install->run.environment = request->environment;
    install->run.platform = runtime_platform_key();
    install->host_id = request->host_id;
    install->plugins = request->plugins;
    return plugins && install->run.portable != NULL && install->run.state != NULL && install->run.cache != NULL &&
           cJSON_IsObject(request->environment) && request->host_id != NULL && install->run.platform != NULL;
}

cJSON *runtime_install_plugins(const runtime_install_plugins_request *request, cJSON **refusal) {
    plugins_install install;
    *refusal = NULL;
    memset(&install, 0, sizeof install);
    if (!is_usable(request, &install)) {
        *refusal = runtime_refusal("the call carries no usable places, environment, host or plugins",
                                   "send {\"places\": {\"portable\", \"state\", \"cache\"}, \"environment\": {<name>: "
                                   "<value>}, \"host\": \"<host id>\", \"plugins\": [<plugin id>]}");
        return NULL;
    }
    install.installed = cJSON_CreateObject();
    install.run.report = install.installed == NULL ? NULL : runtime_install_new_report();
    int done = install.run.report != NULL && read_layers(&install, refusal) && install_all(&install);
    cJSON *report = done ? install.run.report : NULL;
    if (!done) {
        cJSON_Delete(install.run.report);
    }
    cJSON_Delete(install.installed);
    cJSON_Delete(install.own_values);
    runtime_free_loader_layers(&install.layers);
    return report;
}
