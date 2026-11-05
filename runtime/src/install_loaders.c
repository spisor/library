#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "cache_refusal.h"
#include "config.h"
#include "catalog_text.h"
#include "hosts.h"
#include "install_entries.h"
#include "install_hosts.h"
#include "install_references.h"
#include "install_report.h"
#include "install_run.h"
#include "install_settings.h"
#include "install_support.h"
#include "json_service.h"
#include "loader.h"
#include "node_json_file.h"
#include "paths.h"
#include "registry.h"
#include "spisor/path.h"
#include "version_map.h"

#define LOADER_FIX "reinstall the loader from a catalog that lists a valid host.json"

/** One loader the catalog lists for a settled application, and what install learned of it. */
typedef struct catalog_loader {
    const char *id;
    const cJSON *application;
    const char *version;
    cJSON *standard;
    char *home;
} catalog_loader;

static const char *settled_state(const cJSON *report, const char *application) {
    for (const cJSON *row = cJSON_GetObjectItemCaseSensitive(report, "applications")->child; row != NULL; row = row->next) {
        const char *id = runtime_install_text(row, "id");
        const char *state = runtime_install_text(row, "state");
        if (id != NULL && strcmp(id, application) == 0 && state != NULL &&
            (strcmp(state, "installed") == 0 || strcmp(state, "detected") == 0)) {
            return state;
        }
    }
    return NULL;
}

static const cJSON *origin_of(const cJSON *report, const char *application) {
    for (const cJSON *row = cJSON_GetObjectItemCaseSensitive(report, "applications")->child; row != NULL; row = row->next) {
        const char *id = runtime_install_text(row, "id");
        if (id != NULL && strcmp(id, application) == 0) {
            return cJSON_GetObjectItemCaseSensitive(row, "origin");
        }
    }
    return NULL;
}

static int reference_version(const runtime_install_run *run, const char *host_id, const char *id, const char *version,
                             cJSON **refusal) {
    cJSON *versions = cJSON_CreateObject();
    int recorded = versions != NULL && cJSON_AddStringToObject(versions, id, version) != NULL &&
                   runtime_install_reference_versions(run->state, host_id, versions, refusal);
    cJSON_Delete(versions);
    return recorded;
}

/** Installs host id at the catalog's latest, its version referenced first; the version, or NULL when reported. */
static const char *install_host(runtime_install_run *run, const char *id, int *out_of_memory) {
    cJSON *target = runtime_install_target(id);
    const char *version = target == NULL ? NULL : runtime_install_target_version(run->catalog, target, "host");
    cJSON *refusal = NULL;
    *out_of_memory = target == NULL;
    if (version != NULL && !reference_version(run, id, id, version, &refusal)) {
        *out_of_memory = !runtime_install_report_refusal(run->report, id, refusal);
        cJSON_Delete(target);
        return NULL;
    }
    const char *installed = target == NULL ? NULL : runtime_install_item(run->catalog, target, "host", run->cache, run->report, out_of_memory);
    cJSON_Delete(target);
    return installed;
}

static cJSON *standard_call(const runtime_install_run *run, const char *id, const char *version) {
    cJSON *call = runtime_install_places_call(run->places);
    if (call == NULL || cJSON_AddStringToObject(call, "id", id) == NULL ||
        cJSON_AddStringToObject(call, "version", version) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static cJSON *home_call(const runtime_install_run *run, const cJSON *application) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || !runtime_install_copy_into(call, "home", cJSON_GetObjectItemCaseSensitive(application, "home")) ||
        !runtime_install_copy_into(call, "environment", run->environment)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** The provenance already recorded for the application, so a later install never rewrites how it was first found. */
static cJSON *stored_provenance(const runtime_install_run *run, const char *id) {
    cJSON *call = runtime_install_places_call(run->places);
    if (call != NULL && cJSON_AddStringToObject(call, "id", id) == NULL) {
        cJSON_Delete(call);
        call = NULL;
    }
    cJSON *refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_registry_get, call, &refusal);
    cJSON_Delete(refusal);
    cJSON *application = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(answer, "registration"), "application");
    cJSON *provenance = cJSON_DetachItemFromObjectCaseSensitive(application, "provenance");
    cJSON_Delete(answer);
    return provenance;
}

static cJSON *origin_provenance(const runtime_install_run *run, const catalog_loader *loader, const cJSON *origin,
                                cJSON **refusal) {
    cJSON *call = cJSON_CreateObject();
    cJSON *application = call == NULL ? NULL : cJSON_AddObjectToObject(call, "application");
    if (application == NULL || !runtime_install_copy_into(call, "origin", origin) ||
        !runtime_install_copy_into(application, "binary", cJSON_GetObjectItemCaseSensitive(loader->application, "binary")) ||
        cJSON_AddStringToObject(call, "home", loader->home) == NULL ||
        !runtime_install_copy_into(call, "environment", run->environment)) {
        cJSON_Delete(call);
        return NULL;
    }
    return runtime_call_or_refuse(runtime_hosts_provenance, call, refusal);
}

/**
 * @implNote registry.register replaces the registration's application whole, so the stored provenance
 * is carried into it; an identity built without it would erase every later update or remove of the
 * application through its install method.
 */
static cJSON *application_of(const runtime_install_run *run, const catalog_loader *loader, cJSON **refusal) {
    cJSON *application = cJSON_Duplicate(loader->application, 1);
    cJSON *provenance = stored_provenance(run, loader->id);
    const cJSON *origin = origin_of(run->report, runtime_install_text(loader->application, "id"));
    if (provenance == NULL && origin != NULL) {
        provenance = origin_provenance(run, loader, origin, refusal);
        if (provenance == NULL && *refusal != NULL) {
            cJSON_Delete(application);
            return NULL;
        }
    }
    if (application == NULL || (provenance != NULL && !cJSON_AddItemToObject(application, "provenance", provenance))) {
        cJSON_Delete(provenance);
        cJSON_Delete(application);
        return NULL;
    }
    return application;
}

static cJSON *command_of(const runtime_install_run *run, const char *loader_id, const char *name) {
    char *boot = runtime_boot_stub_path(run->cache);
    cJSON *command = boot == NULL ? NULL : cJSON_CreateObject();
    cJSON *arguments = command == NULL ? NULL : cJSON_AddArrayToObject(command, "arguments");
    int built = arguments != NULL && cJSON_AddStringToObject(command, "name", name) != NULL &&
                cJSON_AddStringToObject(command, "target", boot) != NULL &&
                runtime_install_add_text(arguments, RUNTIME_BOOTED_HOST_ID) && runtime_install_add_text(arguments, "launch") &&
                runtime_install_add_text(arguments, loader_id);
    free(boot);
    if (!built) {
        cJSON_Delete(command);
        return NULL;
    }
    return command;
}

static cJSON *commands_of(const runtime_install_run *run, const catalog_loader *loader) {
    const cJSON *path_commands = cJSON_GetObjectItemCaseSensitive(loader->standard, "pathCommands");
    cJSON *commands = cJSON_CreateArray();
    for (const cJSON *name = commands == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(path_commands, "requested")->child;
         name != NULL; name = name->next) {
        cJSON *command = command_of(run, loader->id, name->valuestring);
        if (command == NULL || !cJSON_AddItemToArray(commands, command)) {
            cJSON_Delete(command);
            cJSON_Delete(commands);
            return NULL;
        }
    }
    return commands;
}

static cJSON *identity_of(const runtime_install_run *run, const catalog_loader *loader, cJSON **refusal) {
    const cJSON *minimum = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(loader->standard, "pathCommands"),
                                                            "minimum");
    cJSON *application = application_of(run, loader, refusal);
    cJSON *commands = application == NULL ? NULL : commands_of(run, loader);
    cJSON *identity = commands == NULL ? NULL : cJSON_CreateObject();
    int built = identity != NULL && cJSON_AddStringToObject(identity, "id", loader->id) != NULL &&
                cJSON_AddStringToObject(identity, "version", loader->version) != NULL &&
                cJSON_AddStringToObject(identity, "home", loader->home) != NULL &&
                cJSON_AddItemToObject(identity, "application", application);
    application = built ? NULL : application;
    built = built && cJSON_AddBoolToObject(identity, "requiresCommand", cJSON_IsNumber(minimum) && minimum->valuedouble == 1) != NULL &&
            cJSON_AddItemToObject(identity, "commands", commands);
    commands = built ? NULL : commands;
    cJSON_Delete(application);
    cJSON_Delete(commands);
    if (!built) {
        cJSON_Delete(identity);
        return NULL;
    }
    return identity;
}

static cJSON *register_call(const runtime_install_run *run, const catalog_loader *loader, cJSON **refusal) {
    cJSON *provenance = runtime_install_catalog_provenance(run->cache, loader->id, loader->version, refusal);
    cJSON *identity = provenance == NULL ? NULL : identity_of(run, loader, refusal);
    cJSON *call = identity == NULL ? NULL : runtime_install_places_call(run->places);
    int built = call != NULL && cJSON_AddItemToObject(call, "identity", identity);
    identity = built ? NULL : identity;
    built = built && cJSON_AddItemToObject(call, "provenance", provenance);
    provenance = built ? NULL : provenance;
    built = built && runtime_install_copy_into(call, "environment", run->environment);
    cJSON_Delete(identity);
    cJSON_Delete(provenance);
    if (!built) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static cJSON *entry_loader_of(const catalog_loader *loader) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || cJSON_AddStringToObject(entry, "id", loader->id) == NULL ||
        !runtime_install_copy_into(entry, "application", loader->application) ||
        cJSON_AddStringToObject(entry, "home", loader->home) == NULL) {
        cJSON_Delete(entry);
        return NULL;
    }
    return entry;
}

/**
 * @implNote An entry that cannot be written is a line of the loader's report, never a failure of the
 * loader: its registration and commands already stand, and the launcher starts it without the entry.
 */
static cJSON *entry_of(const runtime_install_run *run, const catalog_loader *loader) {
    cJSON *entry_loader = entry_loader_of(loader);
    cJSON *refusal = NULL;
    cJSON *entry = entry_loader == NULL ? NULL : runtime_install_entry(run->places, entry_loader, &refusal);
    cJSON_Delete(entry_loader);
    if (entry != NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "sequences");
        return entry;
    }
    if (!runtime_is_refusal(refusal)) {
        cJSON_Delete(refusal);
        return NULL;
    }
    cJSON_AddFalseToObject(refusal, "written");
    return refusal;
}

static cJSON *loader_row(const runtime_install_run *run, const catalog_loader *loader, cJSON *registered) {
    cJSON *row = cJSON_CreateObject();
    int built = row != NULL && cJSON_AddStringToObject(row, "id", loader->id) != NULL &&
                runtime_install_copy_into(row, "application", cJSON_GetObjectItemCaseSensitive(loader->application, "id")) &&
                cJSON_AddStringToObject(row, "home", loader->home) != NULL &&
                runtime_install_copy_into(row, "commands", cJSON_GetObjectItemCaseSensitive(registered, "commands")) &&
                runtime_install_copy_into(row, "refusedCommands", cJSON_GetObjectItemCaseSensitive(registered, "refusedCommands")) &&
                cJSON_AddItemToObject(row, "entry", entry_of(run, loader));
    cJSON_Delete(registered);
    if (!built) {
        cJSON_Delete(row);
        return NULL;
    }
    return row;
}

/** Reads the loader's standard and its home; 0 with *refusal set when either cannot be read. */
static int learn(const runtime_install_run *run, catalog_loader *loader, cJSON **refusal) {
    cJSON *answer = runtime_call_or_refuse(runtime_loader_installed, standard_call(run, loader->id, loader->version), refusal);
    loader->standard = cJSON_DetachItemFromObjectCaseSensitive(answer, "standard");
    cJSON_Delete(answer);
    if (loader->standard == NULL) {
        return 0;
    }
    if (loader->application == NULL) {
        loader->application = cJSON_GetObjectItemCaseSensitive(loader->standard, "application");
    }
    cJSON *home = runtime_call_or_refuse(runtime_loader_home, home_call(run, loader->application), refusal);
    const char *path = runtime_install_text(home, "home");
    loader->home = path == NULL ? NULL : runtime_catalog_copy(path);
    cJSON_Delete(home);
    return loader->home != NULL;
}

static int report_loader_refusal(runtime_install_run *run, const char *id, cJSON *refusal, const char *fix) {
    if (refusal != NULL && fix != NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(refusal, "fix");
        cJSON_AddStringToObject(refusal, "fix", fix);
    }
    return refusal == NULL ? 0 : runtime_install_report_refusal(run->report, id, refusal);
}

static const cJSON *first_unlisted_plugin(const runtime_install_run *run, const cJSON *plugins) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(run->catalog, "items");
    for (const cJSON *id = plugins == NULL ? NULL : plugins->child; id != NULL; id = id->next) {
        const char *kind = runtime_install_text(cJSON_GetObjectItemCaseSensitive(items, id->valuestring), "kind");
        if (kind == NULL || strcmp(kind, "plugin") != 0) {
            return id;
        }
    }
    return NULL;
}

static cJSON *unlisted_plugin_refusal(const catalog_loader *loader, const char *plugin) {
    const char *parts[5] = {"the loader ", loader->id, " cannot run without the plugin ", plugin, ", which no catalog lists"};
    char *message = runtime_cache_text(parts, 5);
    cJSON *refusal = message == NULL ? NULL
                                     : runtime_refusal(message, "add a catalog that lists the plugin with forebay-host install "
                                                                "--catalog <path or url>, then install again");
    free(message);
    return refusal;
}

static int is_mentioned_enabled(const cJSON *values, const char *id) {
    const cJSON *plugin = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(values, "plugins"), id);
    return cJSON_HasObjectItem(plugin, "enabled");
}

/** Reports the plugin the user turned off as unavailable, with the fix a launch gives; 0 only when memory runs out. */
static int report_disabled_plugin(runtime_install_run *run, const catalog_loader *loader, const char *id) {
    char *key = runtime_install_enabled_key(id);
    char *file = key == NULL ? NULL : runtime_ecosystem_settings_file_holding(loader->home, run->portable, key);
    free(key);
    const char *message_parts[5] = {"the plugin ", id, " that ", loader->id, " requires is disabled"};
    const char *fix_parts[4] = {"set plugins.", id, ".enabled to true in ", file};
    char *message = file == NULL ? NULL : runtime_cache_text(message_parts, 5);
    char *fix = message == NULL ? NULL : runtime_cache_text(fix_parts, 4);
    int reported = fix != NULL && runtime_install_report_unavailable(run->report, id, message, fix);
    free(fix);
    free(message);
    free(file);
    return reported;
}

/**
 * Enables plugin id in the loader's home when no layer of its settings mentions its enabled, so a plugin the
 * user turned off stays off and is reported instead; 0 only when memory runs out.
 */
static int enable_required_plugin(runtime_install_run *run, const catalog_loader *loader, const char *id) {
    runtime_install_settings settings = {loader->home, run->portable, run->state};
    cJSON *refusal = NULL;
    cJSON *values = runtime_install_settings_values(&settings, 1, 1, &refusal);
    int done;
    if (values == NULL) {
        done = refusal != NULL && runtime_install_report_refusal(run->report, id, refusal);
        refusal = NULL;
    } else if (is_mentioned_enabled(values, id)) {
        done = runtime_install_plugin_enabled(values, id) || report_disabled_plugin(run, loader, id);
    } else if (runtime_install_enable_plugin(&settings, id, "application", &refusal)) {
        done = runtime_install_add_text(cJSON_GetObjectItemCaseSensitive(run->report, "enabled"), id);
    } else {
        done = refusal != NULL && runtime_install_report_refusal(run->report, id, refusal);
        refusal = NULL;
    }
    cJSON_Delete(refusal);
    cJSON_Delete(values);
    return done;
}

/**
 * Installs required plugin id referenced for the loader, then enables it; one the report records as unavailable
 * stays disabled, which a launch refuses by name.
 *
 * @implNote The version is NULL only when memory ran out: first_unlisted_plugin has refused any id that is not a
 * catalog item of kind plugin, the target carries no pin, and the catalog schema requires every item a latest
 * version (ITEM_REQUIRED in catalog_schema.c, checked by check_latest). 0 only when memory runs out.
 */
static int install_required_plugin(runtime_install_run *run, const catalog_loader *loader, const char *id) {
    cJSON *target = runtime_install_target(id);
    const char *version = target == NULL ? NULL : runtime_install_target_version(run->catalog, target, "plugin");
    cJSON *refusal = NULL;
    int out_of_memory = target == NULL;
    const char *installed = NULL;
    if (version != NULL && !reference_version(run, loader->id, id, version, &refusal)) {
        out_of_memory = !runtime_install_report_refusal(run->report, id, refusal);
    } else if (version != NULL) {
        installed = runtime_install_item(run->catalog, target, "plugin", run->cache, run->report, &out_of_memory);
    }
    int done = !out_of_memory && (installed == NULL || (runtime_map_set(run->required_plugins, id, installed) &&
                                                         enable_required_plugin(run, loader, id)));
    cJSON_Delete(target);
    return done;
}

/** Installs and enables every plugin the loader's standard requires; 0 with *refusal set when the catalog lacks one, or with none when memory runs out. */
static int install_required_plugins(runtime_install_run *run, const catalog_loader *loader, cJSON **refusal) {
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(loader->standard, "plugins");
    const cJSON *unlisted = first_unlisted_plugin(run, plugins);
    if (unlisted != NULL) {
        *refusal = unlisted_plugin_refusal(loader, unlisted->valuestring);
        return 0;
    }
    for (const cJSON *id = plugins->child; id != NULL; id = id->next) {
        if (!install_required_plugin(run, loader, id->valuestring)) {
            return 0;
        }
    }
    return 1;
}

/** Registers one installed loader; the background hosts it names once it is, or an empty array. */
static cJSON *register_loader(runtime_install_run *run, catalog_loader *loader) {
    cJSON *refusal = NULL;
    if (!learn(run, loader, &refusal)) {
        return report_loader_refusal(run, loader->id, refusal, loader->standard == NULL ? LOADER_FIX : NULL) ? cJSON_CreateArray()
                                                                                                              : NULL;
    }
    if (!install_required_plugins(run, loader, &refusal)) {
        return report_loader_refusal(run, loader->id, refusal, NULL) ? cJSON_CreateArray() : NULL;
    }
    cJSON *call = register_call(run, loader, &refusal);
    cJSON *registered = call == NULL ? NULL : runtime_call_or_refuse(runtime_hosts_register, call, &refusal);
    if (registered == NULL) {
        return report_loader_refusal(run, loader->id, refusal, NULL) ? cJSON_CreateArray() : NULL;
    }
    if (!reference_version(run, loader->id, loader->id, loader->version, &refusal)) {
        cJSON_Delete(refusal);
    }
    cJSON *row = loader_row(run, loader, registered);
    if (row == NULL || !cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(run->report, "loaders"), row)) {
        cJSON_Delete(row);
        return NULL;
    }
    cJSON *background = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(loader->standard, "background"), 1);
    return background != NULL ? background : cJSON_CreateArray();
}

static int add_unique(cJSON *ids, const cJSON *more) {
    for (const cJSON *id = more == NULL ? NULL : more->child; id != NULL; id = id->next) {
        if (cJSON_IsString(id) && !runtime_install_holds(ids, id->valuestring) && !runtime_install_add_text(ids, id->valuestring)) {
            return 0;
        }
    }
    return 1;
}

static cJSON *loader_background(runtime_install_run *run, const char *id, const cJSON *application) {
    int out_of_memory = 0;
    catalog_loader loader = {id, application, install_host(run, id, &out_of_memory), NULL, NULL};
    cJSON *background = loader.version == NULL ? (out_of_memory ? NULL : cJSON_CreateArray()) : register_loader(run, &loader);
    cJSON_Delete(loader.standard);
    free(loader.home);
    return background;
}

/** How the bundle starts, when its host.json names an entry inside it: {runtime, entry}; NULL otherwise. */
static cJSON *start_of(const runtime_install_run *run, const char *id, const char *version) {
    const char *segments[5] = {run->cache, "bundles", id, version, "host.json"};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 5);
    cJSON *standard = path == NULL ? NULL : runtime_read_node_json(path);
    free(path);
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(run->catalog, "items"), id);
    const char *entry = runtime_install_text(standard, "entry");
    const char *runtime = runtime_install_text(item, "runtime");
    cJSON *start = entry == NULL || runtime == NULL ? NULL : cJSON_CreateObject();
    if (start != NULL && (cJSON_AddStringToObject(start, "runtime", runtime) == NULL ||
                          cJSON_AddStringToObject(start, "entry", entry) == NULL)) {
        cJSON_Delete(start);
        start = NULL;
    }
    cJSON_Delete(standard);
    return start;
}

static int register_background(runtime_install_run *run, const char *id, cJSON *registered) {
    int out_of_memory = 0;
    const char *version = install_host(run, id, &out_of_memory);
    if (version == NULL) {
        return !out_of_memory;
    }
    cJSON *start = start_of(run, id, version);
    cJSON *refusal = NULL;
    int done = runtime_install_register_own_host(run->places, id, version, start, &refusal) &&
               reference_version(run, id, id, version, &refusal);
    cJSON_Delete(start);
    if (!done) {
        return refusal != NULL && runtime_install_report_refusal(run->report, id, refusal);
    }
    return runtime_install_add_text(registered, id);
}

static cJSON *install_background_hosts(runtime_install_run *run, const cJSON *background) {
    cJSON *registered = cJSON_CreateArray();
    for (const cJSON *id = registered == NULL ? NULL : background->child; id != NULL; id = id->next) {
        if (!register_background(run, id->valuestring, registered)) {
            cJSON_Delete(registered);
            return NULL;
        }
    }
    return registered;
}

cJSON *runtime_install_loaders(runtime_install_run *run) {
    cJSON *background = cJSON_CreateArray();
    for (const cJSON *item = background == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(run->catalog, "items")->child;
         item != NULL; item = item->next) {
        const cJSON *application = cJSON_GetObjectItemCaseSensitive(item, "application");
        const char *kind = runtime_install_text(item, "kind");
        const char *application_id = runtime_install_text(application, "id");
        if (kind == NULL || strcmp(kind, "host") != 0 || application_id == NULL || settled_state(run->report, application_id) == NULL) {
            continue;
        }
        cJSON *named = loader_background(run, item->string, application);
        int added = named != NULL && add_unique(background, named);
        cJSON_Delete(named);
        if (!added) {
            cJSON_Delete(background);
            return NULL;
        }
    }
    cJSON *registered = background == NULL ? NULL : install_background_hosts(run, background);
    cJSON_Delete(background);
    return registered;
}

cJSON *runtime_install_register_installed_loader(const runtime_install_run *run, const char *id, const char *version,
                                                 cJSON **refusal) {
    catalog_loader loader = {id, NULL, version, NULL, NULL};
    cJSON *call = learn(run, &loader, refusal) ? register_call(run, &loader, refusal) : NULL;
    cJSON *registered = call == NULL ? NULL : runtime_call_or_refuse(runtime_hosts_register, call, refusal);
    cJSON_Delete(loader.standard);
    free(loader.home);
    return registered;
}
