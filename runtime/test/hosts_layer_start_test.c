#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hosts_call.h"
#include "spisor/allocation.h"
#include "updates_fixture.h"

#include "cache_marker.h"

/**
 * @file
 * Calls hosts' layer_start as launch, action and the UI do, over a registered loader whose settings enable plugins of
 * one runtime or two, and requires each layer's start, its identity and the order they come in.
 */

#define LOADER "test-host"
#define JAVASCRIPT "javascript"
#define NATIVE "native"
#define JAVASCRIPT_LAYER "javascript-host"
#define NATIVE_LAYER "spisor-interface"
#define LAYER_VERSION "2.0.0"
#define NO_CAPABILITIES "{\"provides\":[],\"consumes\":[]}"
#define GREET_ACTION "{\"greet\":{\"call\":{\"service\":\"compiled\",\"method\":\"greet\"}}}"

static char user_home[1400];
static char application_home[1400];
static char portable[1400];
static char node[1400];
static char runtime_binary[1400];

static void write_tree(const char *base, const char *rest, cJSON *tree) {
    char *text = cJSON_PrintUnformatted(tree);
    write_in(base, rest, text);
    cJSON_free(text);
    cJSON_Delete(tree);
}

static void register_loader(void) {
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", LOADER);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddStringToObject(registration, "home", application_home);
    write_tree(state, "hosts/" LOADER ".json", registration);
}

/** Lists plugin `id` at 1.0.0 on `runtime` and installs it with `actions`, a JSON object, unless NULL. */
static void install_plugin_on(const char *id, const char *runtime, const char *actions) {
    char manifest[1024];
    char rest[512];
    list_plugin(id, "1.0.0", "1.0.0");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog_items, id);
    cJSON_DeleteItemFromObjectCaseSensitive(item, "runtime");
    cJSON_AddStringToObject(item, "runtime", runtime);
    mark_installed("bundles", id, "1.0.0");
    snprintf(manifest, sizeof manifest, "{\"id\":\"%s\",\"version\":\"1.0.0\",\"capabilities\":" NO_CAPABILITIES "%s%s}",
             id, actions == NULL ? "" : ",\"actions\":", actions == NULL ? "" : actions);
    snprintf(rest, sizeof rest, "bundles/%s/1.0.0/plugin.json", id);
    write_in(cache, rest, manifest);
}

/** Writes `name` holding `content` into `directory` and records its size in `sizes`. */
static void write_sized(const char *directory, const char *name, const char *content, cJSON *hashes, cJSON *sizes) {
    char *path = joined(directory, name);
    char sha256[NAULA_SHA256_HEX_SIZE];
    write_file(path, content);
    sha256_of_text(content, sha256);
    cJSON_AddStringToObject(hashes, name, sha256);
    cJSON_AddNumberToObject(sizes, name, (double)strlen(content));
    spisor_memory_release(path);
}

/** Installs layer item `layer_id` at LAYER_VERSION, its host.json naming `entry`, and hints it with the runtime binary. */
static void install_layer(const char *layer_id, const char *entry) {
    char rest[512];
    char host_json[256];
    snprintf(rest, sizeof rest, "bundles/%s/" LAYER_VERSION, layer_id);
    char *directory = joined(cache, rest);
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    snprintf(host_json, sizeof host_json, "{\"entry\":\"%s\"}", entry);
    write_sized(directory, "host.json", host_json, hashes, sizes);
    write_sized(directory, entry, "layer", hashes, sizes);
    CHECK(runtime_cache_write_marker(directory, hashes, sizes) == SPISOR_OK);
    cJSON *hint = cJSON_CreateObject();
    cJSON *runtime = cJSON_AddObjectToObject(cJSON_AddObjectToObject(hint, "libraries"), "runtime");
    cJSON_AddStringToObject(runtime, "version", "0.1.0");
    cJSON_AddStringToObject(runtime, "directory", scratch);
    cJSON_AddStringToObject(runtime, "binary", runtime_binary);
    cJSON_AddStringToObject(hint, "host", LAYER_VERSION);
    char *text = cJSON_PrintUnformatted(hint);
    write_hint(layer_id, text);
    cJSON_free(text);
    cJSON_Delete(hint);
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    spisor_memory_release(directory);
}

/** The loader's standard at its registered version, requiring the plugins `required_json` lists. */
static void require_plugins(const char *required_json) {
    char standard[512];
    snprintf(standard, sizeof standard, "{\"plugins\":%s}", required_json);
    write_in(cache, "bundles/" LOADER "/1.0.0/host.json", standard);
}

/** A registered loader whose settings enable `enabled_json`'s plugins, both layer items installed and hinted. */
static void start_world(const char *label, const char *enabled_json) {
    char settings[1024];
    start_base(label);
    snprintf(user_home, sizeof user_home, "%s/user", scratch);
    snprintf(application_home, sizeof application_home, "%s/application", scratch);
    snprintf(portable, sizeof portable, "%s/portable", scratch);
    snprintf(node, sizeof node, "%s/node-bin/node", scratch);
    snprintf(runtime_binary, sizeof runtime_binary, "%s/runtime.dll", scratch);
    write_file(node, "node");
    write_file(runtime_binary, "runtime");
    cJSON *record = cJSON_CreateObject();
    cJSON_AddStringToObject(record, "executable", node);
    write_tree(cache, "runtimes/javascript.json", record);
    register_loader();
    install_layer(JAVASCRIPT_LAYER, "weivel-host.mjs");
    install_layer(NATIVE_LAYER, "spisor-interface");
    snprintf(settings, sizeof settings, "{\"plugins\":%s}", enabled_json);
    write_tree(application_home, "config/settings.json", settings_with(settings));
}

static void finish_world(void) {
    finish_base();
}

static runtime_variable *environment_of(size_t *count) {
    static runtime_variable environment[5];
    environment[0] = (runtime_variable){"HOME", user_home};
    environment[1] = (runtime_variable){"USERPROFILE", user_home};
    environment[2] = (runtime_variable){"SPISOR_HOME", portable};
    environment[3] = (runtime_variable){"SPISOR_STATE", state};
    environment[4] = (runtime_variable){"SPISOR_CACHE", cache};
    *count = 5;
    return environment;
}

/** What one layer_start answered: its status, the layers as JSON, and the refusal's message and fix. */
typedef struct layer_answer {
    spisor_status status;
    cJSON *layers;
    cJSON *refusal;
} layer_answer;

static cJSON *layer_json(const runtime_hosts_layer *layer, size_t expected_environment) {
    cJSON *json = cJSON_CreateObject();
    cJSON *arguments = cJSON_AddArrayToObject(json, "arguments");
    cJSON_AddStringToObject(json, "runtime", layer->runtime);
    cJSON_AddStringToObject(json, "hostId", layer->host_id);
    cJSON_AddStringToObject(json, "executable", layer->executable);
    for (size_t index = 0; index < layer->argument_count; index++) {
        cJSON_AddItemToArray(arguments, cJSON_CreateString(layer->arguments[index]));
    }
    CHECK(layer->environment_count == expected_environment);
    return json;
}

static layer_answer layer_start(uint32_t mode, const char *plugin, const char *action, const char *input) {
    const runtime_hosts_definition *hosts = runtime_hosts_definition_of();
    runtime_hosts_layer_start_argument argument = SPISOR_INIT(runtime_hosts_layer_start_argument);
    runtime_hosts_layer_start_answer answer = SPISOR_INIT(runtime_hosts_layer_start_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    layer_answer result = {0, NULL, NULL};
    write_catalog();
    argument.environment = environment_of(&argument.environment_count);
    argument.loader_id = LOADER;
    argument.mode = mode;
    argument.plugin = plugin;
    argument.action = action;
    argument.input = input;
    result.status = hosts->layer_start(hosts->context, &argument, &answer, &refusal);
    if (result.status == SPISOR_OK) {
        result.layers = cJSON_CreateArray();
        for (size_t index = 0; index < answer.layer_count; index++) {
            cJSON_AddItemToArray(result.layers, layer_json(&answer.layers[index], argument.environment_count));
        }
    } else {
        result.refusal = cache_refusal_tree(&refusal);
    }
    hosts->release_layer_start_answer(hosts->context, &answer);
    hosts->release_refusal(hosts->context, &refusal);
    return result;
}

static void release_answer(layer_answer *answer) {
    cJSON_Delete(answer->layers);
    cJSON_Delete(answer->refusal);
}

/** The bundle path of `layer_id`'s `entry`, as the start names it. */
static char *entry_path(const char *layer_id, const char *entry) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s/" LAYER_VERSION "/%s", layer_id, entry);
    char *path = joined(cache, rest);
    return path;
}

/** The layer {runtime, hostId, executable, arguments} expected for `runtime` under `identity`, its arguments ending in `tail_json`'s. */
static cJSON *expected_layer(const char *runtime, const char *identity, const char *tail_json) {
    int native = strcmp(runtime, NATIVE) == 0;
    char *entry = native ? entry_path(NATIVE_LAYER, "spisor-interface") : entry_path(JAVASCRIPT_LAYER, "weivel-host.mjs");
    cJSON *layer = cJSON_CreateObject();
    cJSON *arguments = cJSON_AddArrayToObject(layer, "arguments");
    cJSON *tail = parsed(tail_json);
    cJSON *each;
    cJSON_AddStringToObject(layer, "runtime", runtime);
    cJSON_AddStringToObject(layer, "hostId", identity);
    cJSON_AddStringToObject(layer, "executable", native ? entry : node);
    cJSON_AddItemToArray(arguments, cJSON_CreateString(native ? "host" : entry));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(runtime_binary));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(runtime));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(LOADER));
    cJSON_ArrayForEach(each, tail) {
        cJSON_AddItemToArray(arguments, cJSON_Duplicate(each, 1));
    }
    cJSON_Delete(tail);
    spisor_memory_release(entry);
    return layer;
}

/** The JSON text of tree with every escaped backslash a slash, so a path compares whichever separator built it. */
static char *slashed_text(const cJSON *tree) {
    char *text = tree == NULL ? NULL : cJSON_PrintUnformatted(tree);
    char *read = text;
    char *written = text;
    while (read != NULL && *read != 0) {
        if (read[0] == '\\' && read[1] == '\\') {
            *written++ = '/';
            read += 2;
        } else {
            *written++ = *read++;
        }
    }
    if (written != NULL) {
        *written = 0;
    }
    return text;
}

static void expect_layers(const layer_answer *answer, cJSON *expected, const char *label) {
    char *wanted = slashed_text(expected);
    char *got = slashed_text(answer->layers);
    if (answer->status != SPISOR_OK || got == NULL || strcmp(wanted, got) != 0) {
        const char *message = text_at(answer->refusal, "message");
        fprintf(stderr, "%s: expected %s, got status %d and %s\n", label, wanted, (int)answer->status,
                got == NULL ? (message == NULL ? "(none)" : message) : got);
        check_failures++;
    }
    cJSON_free(got);
    cJSON_free(wanted);
    cJSON_Delete(expected);
}

static cJSON *layers_of(cJSON *first, cJSON *second) {
    cJSON *layers = cJSON_CreateArray();
    cJSON_AddItemToArray(layers, first);
    if (second != NULL) {
        cJSON_AddItemToArray(layers, second);
    }
    return layers;
}

static void a_javascript_only_loader_answers_one_layer_under_its_own_identity(void) {
    start_world("layer-javascript", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer, layers_of(expected_layer(JAVASCRIPT, LOADER, "[]"), NULL), "the javascript layer");
    release_answer(&answer);
    finish_world();
}

static void a_loader_spanning_two_layers_answers_both_the_front_door_last(void) {
    start_world("layer-two", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    install_plugin_on("compiled", NATIVE, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(NATIVE, LOADER ".native", "[]"), expected_layer(JAVASCRIPT, LOADER, "[]")),
                  "the other layer first, the front door last");
    release_answer(&answer);
    finish_world();
}

static void the_first_required_plugin_puts_the_front_door_in_its_layer(void) {
    start_world("layer-native-door", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    install_plugin_on("compiled", NATIVE, NULL);
    require_plugins("[\"compiled\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(JAVASCRIPT, LOADER ".javascript", "[]"), expected_layer(NATIVE, LOADER, "[]")),
                  "the native layer holds the front door");
    release_answer(&answer);
    finish_world();
}

static void an_in_process_start_ends_each_layer_with_its_word(void) {
    start_world("layer-in-process", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_IN_PROCESS, NULL, NULL, NULL);
    expect_layers(&answer, layers_of(expected_layer(JAVASCRIPT, LOADER, "[\"in-process\"]"), NULL), "in-process");
    release_answer(&answer);
    finish_world();
}

static void an_action_of_a_native_plugin_answers_the_native_layer_alone(void) {
    start_world("layer-action", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    install_plugin_on("compiled", NATIVE, GREET_ACTION);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_ACTION, "compiled", "greet", "{}");
    expect_layers(&answer,
                  layers_of(expected_layer(NATIVE, LOADER ".native", "[\"action\",\"compiled\",\"greet\",\"{}\"]"), NULL),
                  "the action's layer");
    release_answer(&answer);
    finish_world();
}

/**
 * install records the catalog source in spisor-host's own settings alone, so a layer reading the loader's would find
 * no catalog and take the native plugin for a javascript one.
 */
static void a_loader_naming_no_catalog_reads_runtimes_from_spisor_hosts(void) {
    char ecosystem_home[1400];
    start_world("layer-ecosystem-catalog", "{}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    install_plugin_on("compiled", NATIVE, NULL);
    write_in(application_home, "config/settings.json",
             "{\"plugins\":{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}}");
    snprintf(ecosystem_home, sizeof ecosystem_home, "%s/spisor-host", scratch);
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", "spisor-host");
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddStringToObject(registration, "home", ecosystem_home);
    write_tree(state, "hosts/spisor-host.json", registration);
    write_tree(ecosystem_home, "config/settings.json", settings_with("{}"));
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(NATIVE, LOADER ".native", "[]"), expected_layer(JAVASCRIPT, LOADER, "[]")),
                  "the runtimes of spisor-host's catalog");
    release_answer(&answer);
    finish_world();
}

static void a_loader_enabling_no_plugin_answers_no_layer(void) {
    start_world("layer-none", "{}");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer, cJSON_CreateArray(), "no layer");
    release_answer(&answer);
    finish_world();
}

static void a_layer_with_no_hinted_version_is_refused_with_the_reinstall_fix(void) {
    start_world("layer-unhinted", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    install_plugin_on("compiled", NATIVE, NULL);
    write_hint(NATIVE_LAYER, "{\"libraries\":{}}");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    CHECK(answer.status == SPISOR_REFUSED);
    const char *message = text_at(answer.refusal, "message");
    CHECK(message != NULL && strstr(message, "no " NATIVE_LAYER " version is recorded") != NULL);
    check_text(text_at(answer.refusal, "fix"), REINSTALL_FIX, "the reinstall fix");
    release_answer(&answer);
    finish_world();
}

static void a_hint_naming_no_runtime_binary_is_refused_with_the_reinstall_fix(void) {
    start_world("layer-no-binary", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_on("scripted", JAVASCRIPT, NULL);
    write_hint(JAVASCRIPT_LAYER, "{\"libraries\":{},\"host\":\"" LAYER_VERSION "\"}");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    CHECK(answer.status == SPISOR_REFUSED);
    const char *message = text_at(answer.refusal, "message");
    CHECK(message != NULL && strstr(message, "names no runtime binary") != NULL);
    check_text(text_at(answer.refusal, "fix"), REINSTALL_FIX, "the reinstall fix");
    release_answer(&answer);
    finish_world();
}

static void a_mode_or_its_members_out_of_shape_is_malformed(void) {
    start_world("layer-malformed", "{}");
    layer_answer unknown = layer_start(4, NULL, NULL, NULL);
    layer_answer bare_action = layer_start(RUNTIME_HOSTS_LAYER_ACTION, "compiled", NULL, "{}");
    layer_answer serve_with_plugin = layer_start(RUNTIME_HOSTS_LAYER_SERVE, "compiled", NULL, NULL);
    CHECK(unknown.status == SPISOR_MALFORMED);
    CHECK(bare_action.status == SPISOR_MALFORMED);
    CHECK(serve_with_plugin.status == SPISOR_MALFORMED);
    release_answer(&unknown);
    release_answer(&bare_action);
    release_answer(&serve_with_plugin);
    finish_world();
}

int main(void) {
    a_javascript_only_loader_answers_one_layer_under_its_own_identity();
    a_loader_spanning_two_layers_answers_both_the_front_door_last();
    the_first_required_plugin_puts_the_front_door_in_its_layer();
    an_in_process_start_ends_each_layer_with_its_word();
    an_action_of_a_native_plugin_answers_the_native_layer_alone();
    a_loader_naming_no_catalog_reads_runtimes_from_spisor_hosts();
    a_loader_enabling_no_plugin_answers_no_layer();
    a_layer_with_no_hinted_version_is_refused_with_the_reinstall_fix();
    a_hint_naming_no_runtime_binary_is_refused_with_the_reinstall_fix();
    a_mode_or_its_members_out_of_shape_is_malformed();
    return check_summary("hosts_layer_start_test");
}
