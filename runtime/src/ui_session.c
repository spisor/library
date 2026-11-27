#include "ui_session.h"

#include <string.h>

#include "cache_installed.h"
#include "cache_locate.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "catalog_semver.h"
#include "hosts_layers.h"
#include "json_service.h"
#include "plugin_choices.h"
#include "scope_manifest.h"
#include "allocation.h"
#include "spisor/definition.h"

#define MANIFEST_FIX "repair the manifest, or install the plugin again"
#define NOT_INSTALLED_FIX "install it"
#define UNDESCRIBED_DETAIL "it cannot be drawn, and what refused it gave no reason"
#define UNDESCRIBED_FIX "install it again"
#define CATALOG_PROBLEM_ID "catalog"
#define CATALOG_FIX "check the catalog sources spisor-host's settings name, then open the home again"

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int add_row(cJSON *group, const char *id, const char *version) {
    cJSON *row = cJSON_CreateObject();
    int added = row != NULL && cJSON_AddStringToObject(row, "id", id) != NULL &&
                (version == NULL || cJSON_AddStringToObject(row, "version", version) != NULL) &&
                cJSON_AddItemToArray(group, row);
    if (!added) {
        cJSON_Delete(row);
    }
    return added;
}

static int is_listed(const cJSON *rows, const char *id) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        if (strcmp(text_of(row, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Adds {id, version?, detail, fix} to problems, kept in id order; 0 when memory runs out.
 *
 * @implNote A NULL detail or fix is substituted, since cJSON reads NULL text as an allocation that failed.
 */
static int add_problem(cJSON *problems, const char *id, const char *version, const char *detail, const char *fix) {
    cJSON *problem = cJSON_CreateObject();
    cJSON *after = problems->child;
    int built = problem != NULL && cJSON_AddStringToObject(problem, "id", id) != NULL &&
                (version == NULL || cJSON_AddStringToObject(problem, "version", version) != NULL) &&
                cJSON_AddStringToObject(problem, "detail", detail == NULL ? UNDESCRIBED_DETAIL : detail) != NULL &&
                cJSON_AddStringToObject(problem, "fix", fix == NULL ? UNDESCRIBED_FIX : fix) != NULL;
    int index = 0;
    while (after != NULL && strcmp(text_of(after, "id"), id) <= 0) {
        after = after->next;
        index++;
    }
    if (built) {
        built = after == NULL ? cJSON_AddItemToArray(problems, problem) : cJSON_InsertItemInArray(problems, index, problem);
    }
    if (!built) {
        cJSON_Delete(problem);
    }
    return built;
}

/**
 * The manifest at `manifest`, read and checked as the plugin's scope reads and checks it over home and portable,
 * into *description.
 *
 * @return OK; REFUSED with `refused` naming the manifest; OUT_OF_MEMORY.
 */
static spisor_status checked_description(const char *manifest, const char *home, const char *portable,
                                         cJSON **description, spisor_refusal *refused) {
    runtime_scope_manifest read;
    spisor_status status = runtime_read_scope_manifest(manifest, &read, refused);
    *description = NULL;
    if (status == SPISOR_OK) {
        runtime_config_request request = runtime_config_request_of(home, portable);
        request.plugin = read.id;
        request.config = read.config;
        request.settings = read.settings;
        status = runtime_check_scope_plugin(&request, manifest, refused);
    }
    if (status == SPISOR_OK) {
        *description = read.manifest;
        read.manifest = NULL;
    }
    runtime_scope_manifest_free(&read);
    return status;
}

/** Adds the plugin found and its description, which it owns from then on; 0 when memory runs out. */
static int add_plugin(cJSON *plugins, cJSON *descriptions, const cJSON *found, cJSON *description) {
    cJSON *plugin = cJSON_CreateObject();
    int added = plugin != NULL && cJSON_AddStringToObject(plugin, "id", text_of(found, "id")) != NULL &&
                cJSON_AddStringToObject(plugin, "version", text_of(found, "version")) != NULL &&
                cJSON_AddItemToArray(plugins, plugin);
    if (!added) {
        cJSON_Delete(plugin);
    }
    if (!added || !cJSON_AddItemToArray(descriptions, description)) {
        cJSON_Delete(description);
        return 0;
    }
    return 1;
}

/** Adds each problem cache.locate answered; 0 when memory runs out. */
static int add_located_problems(cJSON *problems, const cJSON *located) {
    const cJSON *problem;
    cJSON_ArrayForEach(problem, cJSON_GetObjectItemCaseSensitive(located, "problems")) {
        if (!add_problem(problems, text_of(problem, "id"), text_of(problem, "version"), text_of(problem, "detail"),
                         text_of(problem, "fix"))) {
            return 0;
        }
    }
    return 1;
}

/** Adds a plugin cache.locate found to plugins when its scope reads its manifest, else to problems; 0 on OOM. */
static int add_found(cJSON *plugins, cJSON *descriptions, cJSON *problems, const cJSON *found, const char *home,
                     const char *portable) {
    cJSON *description = NULL;
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    spisor_status status = checked_description(text_of(found, "manifest"), home, portable, &description, &refused);
    int added = status == SPISOR_OK && add_plugin(plugins, descriptions, found, description);
    if (status == SPISOR_REFUSED) {
        added = add_problem(problems, text_of(found, "id"), text_of(found, "version"), refused.detail,
                            refused.fix == NULL ? MANIFEST_FIX : refused.fix);
    }
    spisor_refusal_release(&refused);
    return added;
}

/**
 * Adds to session each plugin of layers the cache locates and its scope reads, in id order, as `plugins` and its
 * checked description as `descriptions`, and each it cannot draw to `problems`; 0 when memory runs out.
 *
 * @implNote The host's run starts without a plugin it cannot locate, and the session draws the rest the same way, so
 * one unreadable plugin never blanks the home; it is listed under problems, since a plugin missing from the home
 * would otherwise read as one never enabled.
 */
static int add_plugins(cJSON *session, cJSON *problems, const runtime_loader_layers *layers, const char *cache,
                       const char *portable) {
    const char *home = text_of(layers->registration, "home");
    cJSON *plugins = cJSON_AddArrayToObject(session, "plugins");
    cJSON *descriptions = cJSON_AddArrayToObject(session, "descriptions");
    cJSON *located = plugins == NULL || descriptions == NULL ? NULL
                                                             : runtime_cache_locate_plugins(cache, layers->plugins);
    const cJSON *found;
    int added = located != NULL && add_located_problems(problems, located);
    const cJSON *found_plugins = added ? cJSON_GetObjectItemCaseSensitive(located, "located") : NULL;
    cJSON_ArrayForEach(found, found_plugins) {
        added = add_found(plugins, descriptions, problems, found, home, portable);
        if (!added) {
            break;
        }
    }
    cJSON_Delete(located);
    return added;
}

/** The version a plugin enabled with none installed would be fetched at, else its pin, else NULL. */
static const char *wanted_version(const cJSON *resolved, const cJSON *candidate) {
    const cJSON *fetch;
    const char *id = text_of(candidate, "id");
    cJSON_ArrayForEach(fetch, cJSON_GetObjectItemCaseSensitive(resolved, "fetch")) {
        if (strcmp(text_of(fetch, "id"), id) == 0) {
            return text_of(fetch, "version");
        }
    }
    return text_of(candidate, "pin");
}

/** Adds to problems each plugin the host's settings enable that has no version installed to run; 0 on OOM. */
static int add_uninstalled(cJSON *problems, const runtime_loader_layers *layers) {
    cJSON *candidates = runtime_plugin_candidates(layers->settings, NULL);
    const cJSON *candidate;
    int added = candidates != NULL;
    cJSON_ArrayForEach(candidate, candidates) {
        const char *id = text_of(candidate, "id");
        const char *parts[2] = {id, " is enabled but not installed"};
        char *detail = cJSON_HasObjectItem(layers->plugins, id) ? NULL : runtime_cache_text(parts, 2);
        added = cJSON_HasObjectItem(layers->plugins, id) ||
                (detail != NULL &&
                 add_problem(problems, id, wanted_version(layers->resolved, candidate), detail, NOT_INSTALLED_FIX));
        spisor_memory_release(detail);
        if (!added) {
            break;
        }
    }
    cJSON_Delete(candidates);
    return added;
}

static const cJSON *items_of(const cJSON *catalog) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    return cJSON_IsObject(items) ? items->child : NULL;
}

static const char *newest_of(const cJSON *versions) {
    const char *newest = NULL;
    const cJSON *version;
    cJSON_ArrayForEach(version, versions) {
        if (cJSON_IsString(version) &&
            (newest == NULL || runtime_catalog_compare_versions(version->valuestring, newest) > 0)) {
            newest = version->valuestring;
        }
    }
    return newest;
}

/**
 * Adds the installed, detected and available groups to session, leaving out of the last two every plugin that runs
 * or is a problem; 0 when memory runs out.
 */
static int add_groups(cJSON *session, const cJSON *plugins, const cJSON *problems, const cJSON *catalog,
                      const char *cache) {
    cJSON *installed = cJSON_AddArrayToObject(session, "installed");
    cJSON *detected = cJSON_AddArrayToObject(session, "detected");
    cJSON *available = cJSON_AddArrayToObject(session, "available");
    int groups = installed != NULL && detected != NULL && available != NULL;
    cJSON *cached = groups ? runtime_cache_installed_bundles(cache) : NULL;
    const cJSON *plugin;
    const cJSON *item;
    int added = cached != NULL;
    for (plugin = added ? plugins->child : NULL; added && plugin != NULL; plugin = plugin->next) {
        added = add_row(installed, text_of(plugin, "id"), text_of(plugin, "version"));
    }
    for (item = added ? items_of(catalog) : NULL; added && item != NULL; item = item->next) {
        const char *kind = runtime_catalog_text_member(item, "kind");
        const cJSON *versions = cJSON_GetObjectItemCaseSensitive(cached, item->string);
        if (kind != NULL && strcmp(kind, "plugin") == 0 && !is_listed(plugins, item->string) &&
            !is_listed(problems, item->string)) {
            added = versions != NULL ? add_row(detected, item->string, newest_of(versions))
                                     : add_row(available, item->string, runtime_catalog_text_member(item, "latest"));
        }
    }
    cJSON_Delete(cached);
    return added;
}

/** The host's ui settings, an empty record for none or one that is not a record; NULL when memory runs out. */
static cJSON *ui_settings(const cJSON *settings) {
    const cJSON *ui = cJSON_GetObjectItemCaseSensitive(settings, "ui");
    return cJSON_IsObject(ui) ? cJSON_Duplicate(ui, 1) : cJSON_CreateObject();
}

/** Adds the problem of sources none of which yields a catalog, when none does; 0 when memory runs out. */
static int add_unyielding(cJSON *problems, const runtime_loader_layers *layers) {
    return layers->yielding > 0 ||
           add_problem(problems, CATALOG_PROBLEM_ID, NULL,
                       "no catalog source yields a catalog, so no plugin the catalog lists can be shown", CATALOG_FIX);
}

/** The session over layers; NULL when memory runs out. */
static cJSON *session_of(const runtime_loader_layers *layers, const char *host_id, const cJSON *places) {
    const char *cache = text_of(places, "cache");
    cJSON *session = cJSON_CreateObject();
    cJSON *problems = cJSON_CreateArray();
    int built = session != NULL && problems != NULL &&
                add_plugins(session, problems, layers, cache, text_of(places, "portable")) &&
                add_uninstalled(problems, layers) &&
                cJSON_AddStringToObject(session, "host", host_id) != NULL &&
                cJSON_AddStringToObject(session, "home", text_of(layers->registration, "home")) != NULL &&
                runtime_add_owned(session, "places", cJSON_Duplicate(places, 1)) &&
                runtime_add_owned(session, "ui", ui_settings(layers->settings)) &&
                add_groups(session, cJSON_GetObjectItemCaseSensitive(session, "plugins"), problems, layers->catalog,
                           cache) &&
                add_unyielding(problems, layers) &&
                runtime_add_owned(session, "sources", cJSON_Duplicate(layers->sources, 1));
    if (built) {
        built = runtime_add_owned(session, "problems", problems);
        problems = NULL;
    }
    cJSON_Delete(problems);
    if (!built) {
        cJSON_Delete(session);
        return NULL;
    }
    return session;
}

cJSON *runtime_ui_session(const cJSON *places, const char *host_id, cJSON **refusal) {
    runtime_loader_layers layers;
    cJSON *session;
    *refusal = NULL;
    if (!runtime_read_loader_layers(places, host_id, &layers, refusal)) {
        return NULL;
    }
    session = session_of(&layers, host_id, places);
    runtime_free_loader_layers(&layers);
    return session;
}
