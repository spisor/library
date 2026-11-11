#include "hosts_start.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_answer.h"
#include "config.h"
#include "file_times.h"
#include "host_references.h"
#include "json_service.h"
#include "node_json_file.h"
#include "paths.h"
#include "places.h"
#include "platform_key.h"
#include "registry.h"
#include "run_records.h"
#include "spisor/json_kind.h"
#include "updates_request.h"
#include "version_map.h"

#define WRITABLE_STATE_FIX "check that SPISOR_STATE names a writable folder, then try again"

typedef struct start_argument {
    const cJSON *environment;
    const char *host_id;
    const char *layer_id;
    const char *layer_version;
    const cJSON *libraries;
    int session;
} start_argument;

/** What a start resolved before it plans: its places, its identity, its settings and what it runs beside its plugins. */
typedef struct start_context {
    start_argument argument;
    cJSON *places;
    const char *state;
    const char *cache;
    const char *portable;
    cJSON *identity;
    cJSON *settings;
    cJSON *running;
} start_context;

static cJSON *argument_refusal(const char *member, const char *shape) {
    char message[256];
    char fix[512];
    snprintf(message, sizeof message, "the argument carries no usable %s", member);
    snprintf(fix, sizeof fix, "send %s as %s", member, shape);
    return runtime_refusal(message, fix);
}

static const char *text_of(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
}

static const char *string_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static int read_argument(const cJSON *argument, start_argument *start, cJSON **refusal) {
    const cJSON *layer = cJSON_GetObjectItemCaseSensitive(argument, "layer");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(argument, "session");
    start->environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    start->host_id = text_of(argument, "hostId");
    start->layer_id = text_of(layer, "id");
    start->layer_version = text_of(layer, "version");
    start->libraries = cJSON_GetObjectItemCaseSensitive(argument, "libraries");
    start->session = cJSON_IsTrue(session);
    if (!runtime_is_version_map(start->environment)) {
        *refusal = argument_refusal("environment", "an object of variable names to string values");
    } else if (start->host_id == NULL || !runtime_is_plain_host_id(start->host_id)) {
        *refusal = argument_refusal("hostId", "the id of the host to start, a plain file name");
    } else if (start->layer_id == NULL || start->layer_version == NULL) {
        *refusal = argument_refusal("layer", "{\"id\": \"<layer bundle id>\", \"version\": \"<its version>\"}");
    } else if (!runtime_is_version_map(start->libraries)) {
        *refusal = argument_refusal("libraries", "an object of the loaded library ids to their versions");
    } else if (!cJSON_IsBool(session)) {
        *refusal = argument_refusal("session", "true for the application's session host, else false");
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
                 runtime_catalog_attach(identity, "application", cJSON_Duplicate(application, 1)));
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
static cJSON *running_of(const start_argument *start, const char *host_version) {
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

static cJSON *library_ids(const cJSON *libraries) {
    cJSON *ids = cJSON_CreateArray();
    for (const cJSON *entry = libraries->child; ids != NULL && entry != NULL; entry = entry->next) {
        if (!cJSON_AddItemToArray(ids, cJSON_CreateString(entry->string))) {
            cJSON_Delete(ids);
            ids = NULL;
        }
    }
    return ids;
}

static cJSON *apply_argument(const start_context *context) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *places = argument == NULL ? NULL : cJSON_AddObjectToObject(argument, "places");
    int built = places != NULL && cJSON_AddStringToObject(places, "state", context->state) != NULL &&
                cJSON_AddStringToObject(places, "cache", context->cache) != NULL &&
                cJSON_AddStringToObject(argument, "hostId", context->argument.host_id) != NULL &&
                cJSON_AddItemReferenceToObject(argument, "settings", context->settings) &&
                cJSON_AddItemReferenceToObject(argument, "running", context->running) &&
                runtime_catalog_attach(argument, "libraries", library_ids(context->argument.libraries));
    if (!built) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

static runtime_updates_request updates_request(const start_context *context, const cJSON *argument) {
    runtime_updates_request request = {argument, context->state, context->cache, context->argument.host_id,
                                       runtime_platform_key()};
    return request;
}

static cJSON *applied(const start_context *context, cJSON **refusal) {
    cJSON *argument = apply_argument(context);
    runtime_updates_request request = updates_request(context, argument);
    cJSON *plan = NULL;
    if (argument != NULL && request.platform == NULL) {
        *refusal = runtime_refusal("this build of the runtime has no platform key, so no version can be resolved for it",
                                   "run a build of the runtime made for this operating system and architecture");
    } else if (argument != NULL) {
        plan = runtime_updates_apply_for(&request, refusal);
    }
    cJSON_Delete(argument);
    return plan;
}

/** 1 when a live run record of any process names host_id, 0 when none does, -1 when memory runs out. */
static int runs_live(const char *state, const char *host_id) {
    cJSON *runs = runtime_live_runs(state, runtime_now_milliseconds());
    if (runs == NULL) {
        return -1;
    }
    int live = 0;
    for (const cJSON *run = runs->child; !live && run != NULL; run = run->next) {
        live = cJSON_HasObjectItem(run, host_id);
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
        if (!cJSON_HasObjectItem(running, entry->string) && !runtime_map_set(plugins, entry->string, entry->valuestring)) {
            cJSON_Delete(plugins);
            plugins = NULL;
        }
    }
    return plugins;
}

static cJSON *reused_plan(const start_context *context, const cJSON *run, cJSON **refusal) {
    char *record = runtime_record_run(context->state, context->argument.host_id, run, RUNTIME_DEFAULT_CHECK_MILLISECONDS);
    if (record == NULL) {
        *refusal = unwritable_run_record(context->argument.host_id);
        return NULL;
    }
    cJSON *plan = cJSON_CreateObject();
    int built = plan != NULL && runtime_catalog_attach(plan, "plugins", plugins_beside(run, context->running)) &&
                runtime_catalog_attach(plan, "run", cJSON_Duplicate(run, 1)) &&
                runtime_catalog_attach(plan, "unrecorded", cJSON_CreateArray()) &&
                cJSON_AddStringToObject(plan, "runRecord", record) != NULL;
    free(record);
    if (!built) {
        cJSON_Delete(plan);
        return NULL;
    }
    return plan;
}

/**
 * What a start that is not the session start runs: the run the host's last start recorded, while a live
 * run record names the host, else what updates.apply answers.
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
    runtime_updates_request request = updates_request(context, NULL);
    cJSON *recorded = runtime_updates_recorded_for(&request, refusal);
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
    return context->argument.session ? applied(context, refusal) : recorded_or_applied(context, reused, refusal);
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

static cJSON *answer_of(const start_context *context, cJSON *plan, int reused) {
    cJSON *answer = cJSON_CreateObject();
    int built = answer != NULL && cJSON_AddStringToObject(answer, "home", text_of(context->places, "home")) != NULL &&
                runtime_catalog_attach(answer, "places", cJSON_Duplicate(context->places, 1)) &&
                runtime_catalog_attach(answer, "identity", cJSON_Duplicate(context->identity, 1)) &&
                runtime_catalog_attach(answer, "settings", cJSON_Duplicate(context->settings, 1)) &&
                runtime_catalog_attach(answer, "plan", plan) &&
                cJSON_AddBoolToObject(answer, "reusedRecordedRun", reused) != NULL &&
                runtime_catalog_attach(answer, "timers", timers()) &&
                cJSON_AddBoolToObject(answer, "noColor",
                                      cJSON_GetObjectItemCaseSensitive(context->argument.environment, "NO_COLOR") != NULL) !=
                    NULL;
    if (!built) {
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
}

static cJSON *start_for(const cJSON *argument, cJSON **refusal) {
    start_context context;
    memset(&context, 0, sizeof context);
    cJSON *answer = NULL;
    if (read_argument(argument, &context.argument, refusal) && prepare(&context, refusal)) {
        int reused = 0;
        cJSON *plan = planned(&context, &reused, refusal);
        answer = plan == NULL ? NULL : answer_of(&context, plan, reused);
    }
    free_context(&context);
    return answer;
}

RUNTIME_EXPORT int32_t runtime_hosts_start(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, start_for);
}
