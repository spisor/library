#include "ui_session.h"

#include <string.h>

#include "cache_installed.h"
#include "cache_locate.h"
#include "cache_refusal.h"
#include "catalog_answer.h"
#include "hosts_layers.h"
#include "json_service.h"
#include "scope_manifest.h"
#include "allocation.h"
#include "spisor/definition.h"

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

/**
 * The manifest at `manifest`, read and checked as the plugin's scope reads and checks it over home and portable;
 * NULL with *refusal set naming it, or with it NULL when memory runs out.
 */
static cJSON *checked_description(const char *manifest, const char *home, const char *portable, cJSON **refusal) {
    runtime_scope_manifest read;
    spisor_refusal refused = SPISOR_INIT(spisor_refusal);
    spisor_status status = runtime_read_scope_manifest(manifest, &read, &refused);
    cJSON *description = NULL;
    if (status == SPISOR_OK) {
        runtime_config_request request = runtime_config_request_of(home, portable);
        request.plugin = read.id;
        request.config = read.config;
        request.settings = read.settings;
        status = runtime_check_scope_plugin(&request, manifest, &refused);
    }
    if (status == SPISOR_OK) {
        description = read.manifest;
        read.manifest = NULL;
    }
    runtime_scope_manifest_free(&read);
    *refusal = status == SPISOR_OK || status == SPISOR_OUT_OF_MEMORY ? NULL : runtime_refusal_of(&refused);
    spisor_refusal_release(&refused);
    return description;
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

/**
 * Whether problems, as cache.locate answers them, hold one, *refusal then set to the first naming its plugin, or left
 * NULL when memory runs out.
 */
static int refused_problem(const cJSON *problems, cJSON **refusal) {
    const cJSON *problem = cJSON_GetArrayItem(problems, 0);
    char *detail;
    *refusal = NULL;
    if (problem == NULL) {
        return 0;
    }
    const char *parts[5] = {text_of(problem, "id"), "@", text_of(problem, "version"), " cannot run: ",
                            text_of(problem, "detail")};
    detail = runtime_cache_text(parts, 5);
    *refusal = detail == NULL ? NULL : runtime_refusal(detail, text_of(problem, "fix"));
    spisor_memory_release(detail);
    return 1;
}

/**
 * Adds to session each plugin of layers the cache locates, in id order, as `plugins` and its checked description as
 * `descriptions`; 0 with *refusal set for a version the cache cannot run or a manifest refused, or with it NULL when
 * memory runs out.
 *
 * @implNote The host's run starts without a version it cannot run, leaving the repair to its update check; the
 * session refuses instead, since a plugin missing from the home would read as one never enabled.
 */
static int add_plugins(cJSON *session, const runtime_loader_layers *layers, const char *cache, const char *portable,
                       cJSON **refusal) {
    const char *home = text_of(layers->registration, "home");
    cJSON *plugins = cJSON_AddArrayToObject(session, "plugins");
    cJSON *descriptions = cJSON_AddArrayToObject(session, "descriptions");
    cJSON *located = plugins == NULL || descriptions == NULL ? NULL
                                                             : runtime_cache_locate_plugins(cache, layers->plugins);
    const cJSON *found;
    int added;
    *refusal = NULL;
    added = located != NULL && !refused_problem(cJSON_GetObjectItemCaseSensitive(located, "problems"), refusal);
    const cJSON *found_plugins = added ? cJSON_GetObjectItemCaseSensitive(located, "located") : NULL;
    cJSON_ArrayForEach(found, found_plugins) {
        cJSON *description = checked_description(text_of(found, "manifest"), home, portable, refusal);
        added = description != NULL && add_plugin(plugins, descriptions, found, description);
        if (!added) {
            break;
        }
    }
    cJSON_Delete(located);
    return added;
}

static int is_session_plugin(const cJSON *plugins, const char *id) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, plugins) {
        if (strcmp(text_of(plugin, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

static const cJSON *items_of(const cJSON *catalog) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    return cJSON_IsObject(items) ? items->child : NULL;
}

/** Adds the installed, detected and available groups to session; 0 when memory runs out. */
static int add_groups(cJSON *session, const cJSON *plugins, const cJSON *catalog, const char *cache) {
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
        if (kind != NULL && strcmp(kind, "plugin") == 0 && !is_session_plugin(plugins, item->string)) {
            added = add_row(cJSON_HasObjectItem(cached, item->string) ? detected : available, item->string,
                            runtime_catalog_text_member(item, "latest"));
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

/** The session over layers; NULL with *refusal set as add_plugins sets it, or with it NULL when memory runs out. */
static cJSON *session_of(const runtime_loader_layers *layers, const char *host_id, const cJSON *places,
                         cJSON **refusal) {
    const char *cache = text_of(places, "cache");
    cJSON *session = cJSON_CreateObject();
    int built = session != NULL && add_plugins(session, layers, cache, text_of(places, "portable"), refusal) &&
                cJSON_AddStringToObject(session, "host", host_id) != NULL &&
                cJSON_AddStringToObject(session, "home", text_of(layers->registration, "home")) != NULL &&
                runtime_add_owned(session, "places", cJSON_Duplicate(places, 1)) &&
                runtime_add_owned(session, "ui", ui_settings(layers->settings)) &&
                add_groups(session, cJSON_GetObjectItemCaseSensitive(session, "plugins"), layers->catalog, cache) &&
                runtime_add_owned(session, "sources", cJSON_Duplicate(layers->sources, 1));
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
    session = session_of(&layers, host_id, places, refusal);
    runtime_free_loader_layers(&layers);
    return session;
}
