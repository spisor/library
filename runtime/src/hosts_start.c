#include "hosts_start.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_locate.h"
#include "cache_folders.h"
#include "cache_refusal.h"
#include "catalog.h"
#include "catalog_answer.h"
#include "config.h"
#include "file_times.h"
#include "host_references.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "places.h"
#include "platform_key.h"
#include "processes_ready.h"
#include "registry.h"
#include "run_records.h"
#include "cJSON.h"
#include "updates.h"
#include "updates_catalog.h"
#include "user_home.h"
#include "version_map.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"

/**
 * What a start resolved before it plans: its places, its identity, its settings and what it runs beside its plugins;
 * and once planned, the identity its run publishes and holds under.
 */
typedef struct start_context {
    runtime_hosts_start_request argument;
    cJSON *places;
    const char *state;
    const char *cache;
    const char *portable;
    cJSON *identity;
    cJSON *settings;
    cJSON *running;
    char *run_id;
} start_context;

static cJSON *request_refusal(const char *member, const char *shape) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the call carries no usable %s", member);
    snprintf(fix, sizeof fix, "pass %s as %s", member, shape);
    return runtime_refusal(message, fix);
}

static int is_nonempty(const char *text) {
    return text != NULL && text[0] != '\0';
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
}

static const char *string_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

int runtime_is_layer_runtime(const char *runtime) {
    return runtime != NULL &&
           (strcmp(runtime, RUNTIME_LAYER_JAVASCRIPT) == 0 || strcmp(runtime, RUNTIME_LAYER_NATIVE) == 0);
}

static int is_usable_request(const runtime_hosts_start_request *start, cJSON **refusal) {
    if (!runtime_is_version_map(start->environment)) {
        *refusal = request_refusal("environment", "an object of variable names to string values");
    } else if (!is_nonempty(start->host_id) || !runtime_is_plain_host_id(start->host_id)) {
        *refusal = request_refusal("hostId", "the id of the host to start, a plain file name");
    } else if (!is_nonempty(start->layer_id) || !is_nonempty(start->layer_version)) {
        *refusal = request_refusal("layer", "the layer bundle's id and its version");
    } else if (!runtime_is_version_map(start->libraries)) {
        *refusal = request_refusal("libraries", "an object of the loaded library ids to their versions");
    } else if (!runtime_is_layer_runtime(start->layer_runtime)) {
        *refusal = request_refusal("layerRuntime", RUNTIME_LAYER_JAVASCRIPT " or " RUNTIME_LAYER_NATIVE);
    }
    return *refusal == NULL;
}

static cJSON *identity_of(const cJSON *registration) {
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(registration, "application");
    cJSON *identity = cJSON_CreateObject();
    int built = identity != NULL &&
                cJSON_AddStringToObject(identity, "version", string_of(registration, "version")) != NULL &&
                cJSON_AddStringToObject(identity, "home", string_of(registration, "home")) != NULL &&
                cJSON_AddBoolToObject(identity, "requiresCommand",
                                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(registration, "requiresCommand"))) != NULL &&
                (!cJSON_IsObject(application) ||
                 runtime_add_owned(identity, "application", cJSON_Duplicate(application, 1)));
    if (!built) {
        cJSON_Delete(identity);
        return NULL;
    }
    return identity;
}

static cJSON *settings_of(const char *home, const char *portable, cJSON **refusal) {
    if (!runtime_is_usable_home(home)) {
        *refusal = runtime_refusal(RUNTIME_UNUSABLE_HOME_MESSAGE, RUNTIME_UNUSABLE_HOME_FIX);
        return NULL;
    }
    int failed = 0;
    return runtime_read_ecosystem_settings(home, NULL, portable, &failed);
}

/** The host at the version it is registered at, the layer and every library: what the start runs beside its plugins. */
static cJSON *running_of(const runtime_hosts_start_request *start, const char *host_version) {
    cJSON *running = cJSON_CreateObject();
    int built = running != NULL && runtime_map_set(running, start->host_id, host_version) &&
                runtime_map_set(running, start->layer_id, start->layer_version) &&
                runtime_map_merge(running, start->libraries);
    if (!built) {
        cJSON_Delete(running);
        return NULL;
    }
    return running;
}

static int prepare(start_context *context, cJSON **refusal) {
    context->places = runtime_places_of_environment(context->argument.environment, refusal);
    context->state = text_of(context->places, "state");
    context->cache = text_of(context->places, "cache");
    context->portable = text_of(context->places, "portable");
    cJSON *registration =
        context->places == NULL ? NULL : runtime_usable_registration(context->state, context->argument.host_id, refusal);
    context->identity = registration == NULL ? NULL : identity_of(registration);
    cJSON_Delete(registration);
    context->settings =
        context->identity == NULL ? NULL : settings_of(string_of(context->identity, "home"), context->portable, refusal);
    context->running =
        context->settings == NULL ? NULL : running_of(&context->argument, string_of(context->identity, "version"));
    return context->running != NULL;
}

static runtime_updates_target updates_target(const start_context *context) {
    runtime_updates_target target = {context->state, context->cache, context->argument.host_id, runtime_platform_key()};
    return target;
}

static cJSON *applied(const start_context *context, cJSON **refusal) {
    runtime_updates_apply_request request = {updates_target(context), context->settings, NULL, context->running, NULL};
    return runtime_updates_apply(&request, refusal);
}

/** 1 when a live run record of any process names host_id, 0 when none does, -1 when memory runs out. */
static int runs_live(const char *state, const char *host_id) {
    cJSON *runs = runtime_live_runs(state, runtime_now_milliseconds());
    if (runs == NULL) {
        return -1;
    }
    int live = 0;
    for (const cJSON *run = runs->child; !live && run != NULL; run = run->next) {
        live = cJSON_GetObjectItemCaseSensitive(run, host_id) != NULL;
    }
    cJSON_Delete(runs);
    return live;
}

static cJSON *unwritable_run_record(const char *host_id) {
    char message[512];
    snprintf(message, sizeof message, "the run record of %s could not be written", host_id);
    return runtime_refusal(message, WRITABLE_STATE_FIX);
}

static cJSON *plugins_beside(const cJSON *run, const cJSON *running) {
    cJSON *plugins = cJSON_CreateObject();
    for (const cJSON *entry = run->child; plugins != NULL && entry != NULL; entry = entry->next) {
        if (cJSON_GetObjectItemCaseSensitive(running, entry->string) == NULL && !runtime_map_set(plugins, entry->string, entry->valuestring)) {
            cJSON_Delete(plugins);
            plugins = NULL;
        }
    }
    return plugins;
}

static cJSON *reused_plan(const start_context *context, const cJSON *run, cJSON **refusal) {
    int out_of_memory = 0;
    spisor_prepared_path removal;
    char *record = runtime_record_run(context->state, context->argument.host_id, run,
                                      RUNTIME_DEFAULT_CHECK_MILLISECONDS, &removal, &out_of_memory);
    if (record == NULL) {
        *refusal = out_of_memory ? NULL : unwritable_run_record(context->argument.host_id);
        return NULL;
    }
    cJSON *plan = cJSON_CreateObject();
    int built = plan != NULL && runtime_add_owned(plan, "plugins", plugins_beside(run, context->running)) &&
                runtime_add_owned(plan, "run", cJSON_Duplicate(run, 1)) &&
                runtime_add_owned(plan, "unrecorded", cJSON_CreateArray()) &&
                cJSON_AddStringToObject(plan, "runRecord", record) != NULL;
    if (!built) {
        spisor_remove_prepared_file(&removal);
        cJSON_Delete(plan);
        plan = NULL;
    }
    spisor_release_prepared_path(&removal);
    spisor_memory_release(record);
    return plan;
}

/**
 * What a start that is not the background session start runs, an in-process one included: the run the host's
 * last start recorded, while a live run record names the host, else what updates.apply answers.
 *
 * @implNote updates.apply may switch the host and its plugins to a scheduled version and rewrite their
 * references, which a second process beside a running session host must not do; a host with no live run
 * has no running session to switch under, so it is applied, which is also what starts a plugin enabled
 * since the last start. The check and the apply are not one step, and need not be: updates.apply holds
 * the registry lock from reading the references to writing them, so a session starting in between applies
 * first, and this apply finds the scheduled version consumed and switches nothing (updates_test,
 * it_converges_two_starts_of_one_host_that_race_on_its_scheduled_version).
 */
static cJSON *recorded_or_applied(const start_context *context, int *reused, cJSON **refusal) {
    runtime_updates_target target = updates_target(context);
    cJSON *recorded = runtime_updates_recorded(&target, refusal);
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(recorded, "run");
    int live = run == NULL || run->child == NULL ? 0 : runs_live(context->state, context->argument.host_id);
    cJSON *plan = NULL;
    if (recorded != NULL && live == 1) {
        *reused = 1;
        plan = reused_plan(context, run, refusal);
    } else if (recorded != NULL && live == 0) {
        plan = applied(context, refusal);
    }
    cJSON_Delete(recorded);
    return plan;
}

static cJSON *planned(const start_context *context, int *reused, cJSON **refusal) {
    *reused = 0;
    return context->argument.session && !context->argument.in_process ? applied(context, refusal)
                                                                      : recorded_or_applied(context, reused, refusal);
}

/** The runtime the catalog lists plugin id on, javascript for a plugin it does not list, as the catalog defaults one. */
static const char *catalog_runtime(const cJSON *catalog, const char *id) {
    const char *runtime = text_of(runtime_updates_item(catalog, id), "runtime");
    return runtime == NULL ? RUNTIME_LAYER_JAVASCRIPT : runtime;
}

static cJSON *strings_of(const cJSON *capabilities, const char *member) {
    const cJSON *strings = cJSON_GetObjectItemCaseSensitive(capabilities, member);
    return cJSON_IsArray(strings) ? cJSON_Duplicate(strings, 1) : cJSON_CreateArray();
}

static const char *located_manifest(const cJSON *located, const char *id) {
    const cJSON *found;
    cJSON_ArrayForEach(found, cJSON_GetObjectItemCaseSensitive(located, "located")) {
        if (strcmp(string_of(found, "id"), id) == 0) {
            return string_of(found, "manifest");
        }
    }
    return NULL;
}

/**
 * {runtime, provides, consumes}: plugin id's catalog runtime and the capabilities its manifest declares, none for a
 * manifest that is not located or cannot be read, which the run refuses by name once it reads it; NULL with
 * *out_of_memory set when memory runs out.
 */
static cJSON *described_plugin(const cJSON *catalog, const char *id, const char *manifest, int *out_of_memory) {
    cJSON *read = manifest == NULL ? NULL : runtime_read_node_json(manifest, out_of_memory);
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(read, "capabilities");
    cJSON *described = *out_of_memory ? NULL : cJSON_CreateObject();
    int built = described != NULL &&
                cJSON_AddStringToObject(described, "runtime", catalog_runtime(catalog, id)) != NULL &&
                runtime_add_owned(described, "provides", strings_of(capabilities, "provides")) &&
                runtime_add_owned(described, "consumes", strings_of(capabilities, "consumes"));
    cJSON_Delete(read);
    if (!built) {
        cJSON_Delete(described);
        *out_of_memory = 1;
        return NULL;
    }
    return described;
}

/** Each of plugins described as described_plugin says, by id; NULL when memory runs out. */
static cJSON *described_plugins(const char *cache, const cJSON *catalog, const cJSON *plugins) {
    cJSON *located = runtime_cache_locate_plugins(cache, plugins);
    cJSON *described = located == NULL ? NULL : cJSON_CreateObject();
    int out_of_memory = described == NULL;
    for (const cJSON *plugin = plugins == NULL ? NULL : plugins->child; !out_of_memory && plugin != NULL; plugin = plugin->next) {
        cJSON *each = described_plugin(catalog, plugin->string, located_manifest(located, plugin->string),
                                       &out_of_memory);
        out_of_memory = out_of_memory || !runtime_add_owned(described, plugin->string, each);
    }
    cJSON_Delete(located);
    if (out_of_memory) {
        cJSON_Delete(described);
        return NULL;
    }
    return described;
}

static int runs_on(const cJSON *plugin, const char *runtime) {
    return strcmp(string_of(plugin, "runtime"), runtime) == 0;
}

static int lists(const cJSON *strings, const char *text) {
    const cJSON *each;
    cJSON_ArrayForEach(each, strings) {
        if (cJSON_IsString(each) && strcmp(each->valuestring, text) == 0) {
            return 1;
        }
    }
    return 0;
}

/** The first described plugin providing capability on runtime when on_runtime is 1, else on any other runtime. */
static const cJSON *provider_of(const cJSON *described, const char *capability, const char *runtime, int on_runtime) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, described) {
        if (runs_on(plugin, runtime) == on_runtime &&
            lists(cJSON_GetObjectItemCaseSensitive(plugin, "provides"), capability)) {
            return plugin;
        }
    }
    return NULL;
}

static int has_plugin_on(const cJSON *described, const char *runtime) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, described) {
        if (runs_on(plugin, runtime)) {
            return 1;
        }
    }
    return 0;
}

/** The ids of every described plugin on runtime, as an object of id to true; NULL when memory runs out. */
static cJSON *layer_plugins(const cJSON *described, const char *runtime) {
    cJSON *needed = cJSON_CreateObject();
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, described) {
        if (needed != NULL && runs_on(plugin, runtime) && cJSON_AddTrueToObject(needed, plugin->string) == NULL) {
            cJSON_Delete(needed);
            needed = NULL;
        }
    }
    return needed;
}

/** Adds to needed every plugin on runtime that provides capability; 0 when memory runs out. */
static int add_providers(cJSON *needed, const cJSON *described, const char *capability, const char *runtime) {
    const cJSON *plugin;
    cJSON_ArrayForEach(plugin, described) {
        int provides = runs_on(plugin, runtime) && lists(cJSON_GetObjectItemCaseSensitive(plugin, "provides"), capability);
        if (provides && !cJSON_HasObjectItem(needed, plugin->string) &&
            cJSON_AddTrueToObject(needed, plugin->string) == NULL) {
            return 0;
        }
    }
    return 1;
}

/**
 * What an action of action_plugin needs, as an object of id to true: it, when it runs on runtime, and every plugin on
 * runtime that provides, transitively, a capability one of them consumes; NULL when memory runs out.
 *
 * @implNote A provider is appended behind the id being read, so the one walk reaches the providers it adds.
 */
static cJSON *action_needs(const cJSON *described, const char *action_plugin, const char *runtime) {
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(described, action_plugin);
    cJSON *needed = cJSON_CreateObject();
    int built = needed != NULL && (action == NULL || !runs_on(action, runtime) ||
                                   cJSON_AddTrueToObject(needed, action_plugin) != NULL);
    for (const cJSON *id = built ? needed->child : NULL; built && id != NULL; id = id->next) {
        const cJSON *capability;
        cJSON_ArrayForEach(capability, cJSON_GetObjectItemCaseSensitive(
                                           cJSON_GetObjectItemCaseSensitive(described, id->string), "consumes")) {
            built = built && (!cJSON_IsString(capability) ||
                              add_providers(needed, described, capability->valuestring, runtime));
        }
    }
    if (!built) {
        cJSON_Delete(needed);
        return NULL;
    }
    return needed;
}

static cJSON *split_refusal(const char *consumer, const char *runtime, const char *capability, const cJSON *provider) {
    const char *parts[10] = {consumer, " (", runtime, ") consumes ", capability, ", which only ", provider->string,
                             " (", string_of(provider, "runtime"), ") provides, and a layer runs plugins of one runtime"};
    const char *fix_parts[4] = {"enable a provider of ", capability, " written in the consumer's language, or disable ",
                                consumer};
    char *fix = runtime_cache_text(fix_parts, 4);
    cJSON *refusal = fix == NULL ? NULL : runtime_cache_refusal(parts, 10, fix);
    spisor_memory_release(fix);
    return refusal;
}

/**
 * 1 with *refusal set, or NULL when memory runs out, when a needed plugin consumes a capability no plugin on runtime
 * provides and a plugin on another runtime does; 0 otherwise.
 */
static int is_split(const cJSON *described, const cJSON *needed, const char *runtime, cJSON **refusal) {
    const cJSON *id;
    cJSON_ArrayForEach(id, needed) {
        const cJSON *capability;
        cJSON_ArrayForEach(capability, cJSON_GetObjectItemCaseSensitive(
                                           cJSON_GetObjectItemCaseSensitive(described, id->string), "consumes")) {
            const char *name = cJSON_GetStringValue(capability);
            const cJSON *elsewhere = name == NULL || provider_of(described, name, runtime, 1) != NULL
                                         ? NULL
                                         : provider_of(described, name, runtime, 0);
            if (elsewhere != NULL) {
                *refusal = split_refusal(id->string, runtime, name, elsewhere);
                return 1;
            }
        }
    }
    return 0;
}

/** plugins less every id needed does not name; NULL when memory runs out. */
static cJSON *narrowed(const cJSON *plugins, const cJSON *needed) {
    cJSON *kept = cJSON_CreateObject();
    for (const cJSON *plugin = plugins == NULL ? NULL : plugins->child; kept != NULL && plugin != NULL; plugin = plugin->next) {
        if (cJSON_HasObjectItem(needed, plugin->string) && !runtime_map_set(kept, plugin->string, plugin->valuestring)) {
            cJSON_Delete(kept);
            kept = NULL;
        }
    }
    return kept;
}

/**
 * The loader standard the host's bundle at the version plan runs carries, NULL when it cannot be read, with
 * *out_of_memory set when memory ran out.
 *
 * @implNote Read as JSON alone, not checked as loader.installed checks it: install refuses a loader whose standard
 * fails that check, and a start reads no more of it than its required plugins.
 */
static cJSON *installed_standard(const start_context *context, const cJSON *plan, int *out_of_memory) {
    const char *version = string_of(cJSON_GetObjectItemCaseSensitive(plan, "run"), context->argument.host_id);
    const char *segments[5] = {context->cache, "bundles", context->argument.host_id,
                               version == NULL ? string_of(context->identity, "version") : version, "host.json"};
    char *file = runtime_cache_joined(segments, 5);
    cJSON *standard = file == NULL ? NULL : runtime_read_node_json(file, out_of_memory);
    *out_of_memory = *out_of_memory || file == NULL;
    spisor_memory_release(file);
    return standard;
}

/**
 * The runtime of the layer holding the front door: that of the first plugin the loader standard requires, else
 * javascript unless only native plugins are described; NULL when memory runs out. A host whose standard cannot be
 * read requires no plugin here, as a start needs none.
 */
static const char *front_door(const start_context *context, const cJSON *catalog, const cJSON *described,
                              const cJSON *plan) {
    int out_of_memory = 0;
    cJSON *standard = installed_standard(context, plan, &out_of_memory);
    const cJSON *required = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(standard, "plugins"), 0);
    const char *door = cJSON_IsString(required) ? catalog_runtime(catalog, required->valuestring) : NULL;
    cJSON_Delete(standard);
    if (door == NULL && !out_of_memory) {
        int native_only =
            !has_plugin_on(described, RUNTIME_LAYER_JAVASCRIPT) && has_plugin_on(described, RUNTIME_LAYER_NATIVE);
        door = native_only ? RUNTIME_LAYER_NATIVE : RUNTIME_LAYER_JAVASCRIPT;
    }
    return door;
}

static char *run_id_of(const char *host_id, const char *runtime, const char *door) {
    const char *parts[3] = {host_id, ".", runtime};
    return runtime_cache_text(parts, strcmp(door, runtime) == 0 ? 1 : 3);
}

static cJSON *current_catalog(const start_context *context, cJSON **refusal) {
    cJSON *sources = runtime_updates_sources(context->settings, refusal);
    cJSON *catalog = sources == NULL ? NULL : runtime_catalog_current(sources, context->cache, refusal);
    cJSON_Delete(sources);
    return catalog;
}

/**
 * Narrows plan's plugins to what the start's layer runs and sets the run's identity; 0 with *refusal set as
 * hosts.start says, or with it NULL when memory runs out.
 */
static int layered(start_context *context, cJSON *plan, cJSON **refusal) {
    const char *runtime = context->argument.layer_runtime;
    const char *action_plugin = context->argument.action_plugin;
    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(plan, "plugins");
    cJSON *catalog = current_catalog(context, refusal);
    cJSON *described = catalog == NULL ? NULL : described_plugins(context->cache, catalog, plugins);
    const char *door = described == NULL ? NULL : front_door(context, catalog, described, plan);
    context->run_id = door == NULL ? NULL : run_id_of(context->argument.host_id, runtime, door);
    cJSON *needed = context->run_id == NULL ? NULL
                    : action_plugin == NULL ? layer_plugins(described, runtime)
                                            : action_needs(described, action_plugin, runtime);
    int split = needed != NULL && is_split(described, needed, runtime, refusal);
    cJSON *kept = needed == NULL || split ? NULL : narrowed(plugins, needed);
    int replaced = kept != NULL && cJSON_ReplaceItemInObjectCaseSensitive(plan, "plugins", kept);
    if (kept != NULL && !replaced) {
        cJSON_Delete(kept);
    }
    cJSON_Delete(needed);
    cJSON_Delete(described);
    cJSON_Delete(catalog);
    return replaced;
}

static cJSON *timers(void) {
    cJSON *timers = cJSON_CreateObject();
    int built = timers != NULL &&
                cJSON_AddNumberToObject(timers, "recordRunMilliseconds", RUNTIME_DEFAULT_CHECK_MILLISECONDS) != NULL &&
                cJSON_AddNumberToObject(timers, "pruneFailuresBeforeExit", RUNTIME_PRUNE_FAILURES_BEFORE_EXIT) != NULL &&
                cJSON_AddNumberToObject(timers, "updateCheckMilliseconds", RUNTIME_UPDATE_CHECK_MILLISECONDS) != NULL;
    if (!built) {
        cJSON_Delete(timers);
        return NULL;
    }
    return timers;
}

/** Whether environment names NO_COLOR at all, empty included, as the OS matches variable names. */
static int carries_no_color(const cJSON *environment) {
    return runtime_environment_variable(environment, "NO_COLOR", RUNTIME_HOST_IS_WINDOWS) != NULL;
}

/** The registration's identity at the version plan runs the host at, else the one it is registered at. */
static cJSON *running_identity(const start_context *context, const cJSON *plan) {
    const char *planned_version = string_of(cJSON_GetObjectItemCaseSensitive(plan, "run"), context->argument.host_id);
    cJSON *identity = cJSON_Duplicate(context->identity, 1);
    cJSON *version = identity == NULL || planned_version == NULL ? NULL : cJSON_CreateString(planned_version);
    if (planned_version != NULL &&
        (version == NULL || !cJSON_ReplaceItemInObjectCaseSensitive(identity, "version", version))) {
        cJSON_Delete(version);
        cJSON_Delete(identity);
        return NULL;
    }
    return identity;
}

/**
 * The answer of a start that planned plan, taking plan. When the answer cannot be built the run record plan wrote is
 * removed before plan is freed, so a start answers OUT_OF_MEMORY only with no run record of its own left.
 *
 * @implNote plan is attached last under a constant key, which allocates nothing, so nothing can fail after it.
 */
static cJSON *answer_of(const start_context *context, cJSON *plan, int reused) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && runtime_add_owned(answer, "places", cJSON_Duplicate(context->places, 1)) &&
                runtime_add_owned(answer, "identity", running_identity(context, plan)) &&
                runtime_add_owned(answer, "settings", cJSON_Duplicate(context->settings, 1)) &&
                cJSON_AddBoolToObject(answer, "reusedRecordedRun", reused) != NULL &&
                (!context->argument.session || runtime_add_owned(answer, "timers", timers())) &&
                cJSON_AddBoolToObject(answer, "noColor", carries_no_color(context->argument.environment)) != NULL &&
                cJSON_AddStringToObject(answer, "runId", context->run_id) != NULL &&
                cJSON_AddItemToObjectCS(answer, "plan", plan);
    if (!built) {
        runtime_hosts_start_undo(plan);
        cJSON_Delete(plan);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static void free_context(start_context *context) {
    cJSON_Delete(context->places);
    cJSON_Delete(context->identity);
    cJSON_Delete(context->settings);
    cJSON_Delete(context->running);
    spisor_memory_release(context->run_id);
}

/**
 * Publishes a session start's refusal to its ready record, so a launch waiting on the host reads why it
 * will not serve rather than timing out.
 *
 * @implNote Best effort: the start is refused either way, and a publication that fails must not hide why.
 * A refusal before the places resolve has nowhere to publish, and a start that is not the session start
 * has no launch waiting on it. A refusal before the run's identity is known is one of every layer alike, so the
 * host's own record, which the front door's layer publishes, carries it.
 */
static void publish_refusal(const start_context *context, const cJSON *refusal) {
    if (context->argument.session && !context->argument.in_process && context->state != NULL && refusal != NULL) {
        runtime_publish_ready_refusal(context->state,
                                      context->run_id == NULL ? context->argument.host_id : context->run_id, refusal);
    }
}

cJSON *runtime_hosts_start(const runtime_hosts_start_request *request, cJSON **refusal) {
    start_context context;
    memset(&context, 0, sizeof context);
    cJSON *answer = NULL;
    *refusal = NULL;
    context.argument = *request;
    if (is_usable_request(request, refusal) && prepare(&context, refusal)) {
        int reused = 0;
        cJSON *plan = planned(&context, &reused, refusal);
        if (plan != NULL && !layered(&context, plan, refusal)) {
            runtime_hosts_start_undo(plan);
            cJSON_Delete(plan);
            plan = NULL;
        }
        answer = plan == NULL ? NULL : answer_of(&context, plan, reused);
    }
    if (answer == NULL) {
        publish_refusal(&context, *refusal);
    }
    free_context(&context);
    return answer;
}


void runtime_hosts_start_undo(const cJSON *plan) {
    spisor_remove_file_in_place(string_of(plan, "runRecord"));
}
