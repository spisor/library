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
 * one language or several, and requires each layer's start, its identity and the order they come in. A layer is a
 * language and its start the language-host item the catalog lists for it: TypeScript runs on javascript, and the two
 * fixture languages each run their own native host, so two languages on one runtime are two layers.
 */

#define LOADER "test-host"
#define TYPESCRIPT "typescript"
#define FIXTURE_ONE "fixture-one"
#define FIXTURE_TWO "fixture-two"
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

/** Lists plugin `id` at 1.0.0 written in `language` and installs it with `actions`, a JSON object, unless NULL. */
static void install_plugin_in(const char *id, const char *language, const char *actions) {
    char manifest[1024];
    char rest[512];
    list_plugin(id, "1.0.0", "1.0.0");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog_items, id);
    cJSON_ReplaceItemInObjectCaseSensitive(item, "language", cJSON_CreateString(language));
    mark_installed("bundles", id, "1.0.0");
    snprintf(manifest, sizeof manifest,
             "{\"id\":\"%s\",\"version\":\"1.0.0\",\"language\":\"%s\",\"capabilities\":" NO_CAPABILITIES "%s%s}",
             id, language, actions == NULL ? "" : ",\"actions\":", actions == NULL ? "" : actions);
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

/** The entry `<language>-host`'s bundle names, its own for each language, so a start names which item it read. */
static void entry_of(const char *language, char *entry, size_t size) {
    snprintf(entry, size, strcmp(language, TYPESCRIPT) == 0 ? "weivel-host.mjs" : "%s-program", language);
}

/**
 * Lists `<language>-host` as a language-host item started under `runtime`, installs it at LAYER_VERSION, its host.json
 * naming its entry and, when `in_process` is 1, an in-process entry, and hints it with the runtime binary.
 */
static void install_language_host(const char *language, const char *runtime, int in_process) {
    char layer_id[128];
    char rest[512];
    char host_json[256];
    char entry[128];
    snprintf(layer_id, sizeof layer_id, "%s-host", language);
    entry_of(language, entry, sizeof entry);
    cJSON *item = cJSON_AddObjectToObject(catalog_items, layer_id);
    cJSON_AddStringToObject(item, "kind", "language-host");
    cJSON_AddStringToObject(item, "language", language);
    cJSON_AddStringToObject(item, "runtime", runtime);
    cJSON_AddStringToObject(item, "latest", LAYER_VERSION);
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(item, "versions"), LAYER_VERSION), "files");
    snprintf(rest, sizeof rest, "bundles/%s/" LAYER_VERSION, layer_id);
    char *directory = joined(cache, rest);
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    snprintf(host_json, sizeof host_json, "{\"entry\":\"%s\"%s}", entry,
             in_process ? ",\"inProcessEntry\":\"in-process.mjs\"" : "");
    write_sized(directory, "host.json", host_json, hashes, sizes);
    write_sized(directory, entry, "layer", hashes, sizes);
    CHECK(runtime_cache_write_marker(directory, hashes, sizes) == SPISOR_OK);
    cJSON *hint = cJSON_CreateObject();
    cJSON *library = cJSON_AddObjectToObject(cJSON_AddObjectToObject(hint, "libraries"), "runtime");
    cJSON_AddStringToObject(library, "version", "0.1.0");
    cJSON_AddStringToObject(library, "directory", scratch);
    cJSON_AddStringToObject(library, "binary", runtime_binary);
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

/**
 * A registered loader whose settings enable `enabled_json`'s plugins, with the language-host items of TypeScript, on
 * javascript and carrying an in-process entry, and of both fixture languages, each native, installed and hinted.
 */
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
    install_language_host(TYPESCRIPT, "javascript", 1);
    install_language_host(FIXTURE_ONE, "native", 0);
    install_language_host(FIXTURE_TWO, "native", 0);
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
    cJSON_AddStringToObject(json, "language", layer->language);
    cJSON_AddStringToObject(json, "hostId", layer->host_id);
    cJSON_AddStringToObject(json, "executable", layer->executable);
    for (size_t index = 0; index < layer->argument_count; index++) {
        cJSON_AddItemToArray(arguments, cJSON_CreateString(layer->arguments[index]));
    }
    CHECK(layer->environment_count == expected_environment);
    return json;
}

static layer_answer layer_start_with(uint32_t mode, const char *plugin, const char *action, const char *input,
                                    const char *result_path, uint32_t size) {
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
    argument.result = result_path;
    argument.size = size;
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

static layer_answer layer_start(uint32_t mode, const char *plugin, const char *action, const char *input) {
    return layer_start_with(mode, plugin, action, input, NULL, (uint32_t)sizeof(runtime_hosts_layer_start_argument));
}

static void release_answer(layer_answer *answer) {
    cJSON_Delete(answer->layers);
    cJSON_Delete(answer->refusal);
}

/** The path of `language`'s host's entry in its bundle, as the start names it. */
static char *entry_path(const char *language) {
    char rest[512];
    char entry[128];
    entry_of(language, entry, sizeof entry);
    snprintf(rest, sizeof rest, "bundles/%s-host/" LAYER_VERSION "/%s", language, entry);
    return joined(cache, rest);
}

/**
 * The layer {language, hostId, executable, arguments} expected for `language` under `identity`, its arguments ending
 * in `tail_json`'s: TypeScript's host run by Node with its entry first, a fixture language's entry run itself.
 */
static cJSON *expected_layer(const char *language, const char *identity, const char *tail_json) {
    int javascript = strcmp(language, TYPESCRIPT) == 0;
    char *entry = entry_path(language);
    cJSON *layer = cJSON_CreateObject();
    cJSON *arguments = cJSON_AddArrayToObject(layer, "arguments");
    cJSON *tail = parsed(tail_json);
    cJSON *each;
    cJSON_AddStringToObject(layer, "language", language);
    cJSON_AddStringToObject(layer, "hostId", identity);
    cJSON_AddStringToObject(layer, "executable", javascript ? node : entry);
    if (javascript) {
        cJSON_AddItemToArray(arguments, cJSON_CreateString(entry));
    }
    cJSON_AddItemToArray(arguments, cJSON_CreateString(runtime_binary));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(language));
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

static void expect_refused(const layer_answer *answer, const char *message_part, const char *fix, const char *label) {
    const char *message = text_at(answer->refusal, "message");
    if (answer->status != SPISOR_REFUSED || message == NULL || strstr(message, message_part) == NULL) {
        fprintf(stderr, "%s: expected a refusal naming \"%s\", got status %d and \"%s\"\n", label, message_part,
                (int)answer->status, message == NULL ? "(none)" : message);
        check_failures++;
    }
    check_text(text_at(answer->refusal, "fix"), fix, label);
}

static void a_typescript_only_loader_answers_one_layer_under_its_own_identity(void) {
    start_world("layer-typescript", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer, layers_of(expected_layer(TYPESCRIPT, LOADER, "[]"), NULL), "the typescript layer");
    release_answer(&answer);
    finish_world();
}

static void a_loader_spanning_two_languages_answers_both_the_front_door_first(void) {
    start_world("layer-two", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    require_plugins("[\"scripted\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(TYPESCRIPT, LOADER, "[]"),
                            expected_layer(FIXTURE_ONE, LOADER "." FIXTURE_ONE, "[]")),
                  "the front door first, the other layer after it");
    release_answer(&answer);
    finish_world();
}

/**
 * The two fixture languages both run on native, so a layer looked up by its runtime would start one item for both;
 * each layer is its own language's item, read from that item's own hint and bundle.
 */
static void two_languages_on_one_runtime_answer_two_layers_each_from_its_own_item(void) {
    start_world("layer-one-runtime", "{\"first\":{\"enabled\":true},\"second\":{\"enabled\":true}}");
    install_plugin_in("first", FIXTURE_ONE, NULL);
    install_plugin_in("second", FIXTURE_TWO, NULL);
    require_plugins("[\"first\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(FIXTURE_ONE, LOADER, "[]"),
                            expected_layer(FIXTURE_TWO, LOADER "." FIXTURE_TWO, "[]")),
                  "one layer per language, each its own item's entry");
    release_answer(&answer);
    finish_world();
}

static void the_first_required_plugin_puts_the_front_door_in_its_layer(void) {
    start_world("layer-fixture-door", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    require_plugins("[\"compiled\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_layers(&answer,
                  layers_of(expected_layer(FIXTURE_ONE, LOADER, "[]"),
                            expected_layer(TYPESCRIPT, LOADER "." TYPESCRIPT, "[]")),
                  "the fixture layer holds the front door");
    release_answer(&answer);
    finish_world();
}

static void a_loader_requiring_no_plugin_over_two_languages_is_refused_naming_both(void) {
    start_world("layer-two-doors", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "so no one language holds its front door",
                   "require the plugin that holds the front door in the loader's standard, or enable plugins of one "
                   "language only, then install the loader again",
                   "no language holds the front door");
    const char *message = text_at(answer.refusal, "message");
    CHECK(message != NULL && strstr(message, TYPESCRIPT) != NULL && strstr(message, FIXTURE_ONE) != NULL);
    release_answer(&answer);
    finish_world();
}

static void a_language_the_catalog_hosts_in_no_item_is_refused_naming_it(void) {
    start_world("layer-unhosted", "{\"spoken\":{\"enabled\":true}}");
    install_plugin_in("spoken", "lua", NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "plugins written in lua have no host: no catalog source lists lua-host",
                   "add a catalog source listing lua-host, or disable the plugins written in lua",
                   "the language with no host item");
    release_answer(&answer);
    finish_world();
}

/** jvm is a runtime the catalog names, so its item is not to be reinstalled: no start exists for it yet. */
static void a_language_host_on_jvm_is_refused_saying_no_jvm_start_exists(void) {
    start_world("layer-jvm", "{\"steeped\":{\"enabled\":true}}");
    install_language_host("kettle", "jvm", 0);
    install_plugin_in("steeped", "kettle", NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "the start recorded for kettle-host runs on jvm, and no jvm start exists yet",
                   "use a host for this language that runs on native or javascript until a jvm start is built",
                   "the jvm layer");
    release_answer(&answer);
    finish_world();
}

static void an_in_process_start_ends_its_layer_with_its_word(void) {
    start_world("layer-in-process", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_IN_PROCESS, NULL, NULL, NULL);
    expect_layers(&answer, layers_of(expected_layer(TYPESCRIPT, LOADER, "[\"in-process\"]"), NULL), "in-process");
    release_answer(&answer);
    finish_world();
}

/** An in-process host runs inside another application's process, so it runs the front door's layer alone. */
static void an_in_process_start_of_two_languages_answers_the_front_doors_layer_alone(void) {
    start_world("layer-in-process-two", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    require_plugins("[\"scripted\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_IN_PROCESS, NULL, NULL, NULL);
    expect_layers(&answer, layers_of(expected_layer(TYPESCRIPT, LOADER, "[\"in-process\"]"), NULL),
                  "the front door's layer alone");
    release_answer(&answer);
    finish_world();
}

static void an_in_process_start_whose_front_doors_host_has_no_in_process_entry_is_refused(void) {
    start_world("layer-in-process-native", "{\"compiled\":{\"enabled\":true}}");
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_IN_PROCESS, NULL, NULL, NULL);
    expect_refused(&answer, FIXTURE_ONE "-host, which hosts " FIXTURE_ONE ", the language of the loader's front door, "
                            "carries no inProcessEntry",
                   "start the loader with spisor-host launch, which runs each layer as a process of its own",
                   "no in-process entry");
    release_answer(&answer);
    finish_world();
}

static void a_first_required_plugin_listed_in_no_language_is_refused_naming_it(void) {
    start_world("layer-unlisted-door", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    require_plugins("[\"brewed\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "brewed, the first plugin the loader requires, is listed in no catalog source as a plugin",
                   "add a catalog source listing brewed, then install the loader again", "the unlisted front door");
    release_answer(&answer);
    finish_world();
}

static void an_action_of_a_fixture_plugin_answers_its_layer_alone(void) {
    start_world("layer-action", "{\"scripted\":{\"enabled\":true},\"compiled\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, GREET_ACTION);
    require_plugins("[\"scripted\"]");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_ACTION, "compiled", "greet", "{}");
    expect_layers(&answer,
                  layers_of(expected_layer(FIXTURE_ONE, LOADER "." FIXTURE_ONE,
                                           "[\"action\",\"compiled\",\"greet\",\"{}\"]"),
                            NULL),
                  "the action's layer");
    release_answer(&answer);
    finish_world();
}

static void an_action_of_a_plugin_listed_in_no_language_is_refused_naming_it(void) {
    start_world("layer-action-unlisted", "{\"compiled\":{\"enabled\":true}}");
    install_plugin_in("compiled", FIXTURE_ONE, GREET_ACTION);
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_ACTION, "absent", "greet", "{}");
    expect_refused(&answer, "absent is listed in no catalog source as a plugin, so no layer runs it",
                   "add a catalog source listing absent, then run the action again", "the unlisted action");
    release_answer(&answer);
    finish_world();
}

static void an_action_given_a_result_path_ends_its_start_with_it(void) {
    start_world("layer-action-result", "{\"compiled\":{\"enabled\":true}}");
    install_plugin_in("compiled", FIXTURE_ONE, GREET_ACTION);
    layer_answer answer = layer_start_with(RUNTIME_HOSTS_LAYER_ACTION, "compiled", "greet", "{}", "/state/results/7-1.json",
                                           (uint32_t)sizeof(runtime_hosts_layer_start_argument));
    expect_layers(&answer,
                  layers_of(expected_layer(FIXTURE_ONE, LOADER,
                                           "[\"action\",\"compiled\",\"greet\",\"{}\",\"/state/results/7-1.json\"]"),
                            NULL),
                  "the action's layer with its result path");
    release_answer(&answer);
    answer = layer_start_with(RUNTIME_HOSTS_LAYER_ACTION, "compiled", "greet", "{}", "/state/results/7-1.json",
                              (uint32_t)SPISOR_SHAPE_THROUGH(runtime_hosts_layer_start_argument, input));
    expect_layers(&answer,
                  layers_of(expected_layer(FIXTURE_ONE, LOADER, "[\"action\",\"compiled\",\"greet\",\"{}\"]"), NULL),
                  "a result past the argument's size is not read");
    release_answer(&answer);
    finish_world();
}

/**
 * install records the catalog source in spisor-host's own settings alone, so a layer reading the loader's would find
 * no catalog and no plugin's language.
 */
static void a_loader_naming_no_catalog_reads_languages_from_spisor_hosts(void) {
    char ecosystem_home[1400];
    start_world("layer-ecosystem-catalog", "{}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    require_plugins("[\"scripted\"]");
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
                  layers_of(expected_layer(TYPESCRIPT, LOADER, "[]"),
                            expected_layer(FIXTURE_ONE, LOADER "." FIXTURE_ONE, "[]")),
                  "the languages of spisor-host's catalog");
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
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    install_plugin_in("compiled", FIXTURE_ONE, NULL);
    require_plugins("[\"scripted\"]");
    write_hint(FIXTURE_ONE "-host", "{\"libraries\":{}}");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "no " FIXTURE_ONE "-host version is recorded", REINSTALL_FIX, "the unhinted layer");
    release_answer(&answer);
    finish_world();
}

static void a_hint_naming_no_runtime_binary_is_refused_with_the_reinstall_fix(void) {
    start_world("layer-no-binary", "{\"scripted\":{\"enabled\":true}}");
    install_plugin_in("scripted", TYPESCRIPT, NULL);
    write_hint(TYPESCRIPT "-host", "{\"libraries\":{},\"host\":\"" LAYER_VERSION "\"}");
    layer_answer answer = layer_start(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL);
    expect_refused(&answer, "names no runtime binary", REINSTALL_FIX, "the hint without a binary");
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
    layer_answer serve_with_result =
        layer_start_with(RUNTIME_HOSTS_LAYER_SERVE, NULL, NULL, NULL, "/state/results/7-1.json",
                         (uint32_t)sizeof(runtime_hosts_layer_start_argument));
    layer_answer empty_result = layer_start_with(RUNTIME_HOSTS_LAYER_ACTION, "compiled", "greet", "{}", "",
                                                 (uint32_t)sizeof(runtime_hosts_layer_start_argument));
    CHECK(serve_with_result.status == SPISOR_MALFORMED);
    CHECK(empty_result.status == SPISOR_MALFORMED);
    release_answer(&serve_with_result);
    release_answer(&empty_result);
    release_answer(&unknown);
    release_answer(&bare_action);
    release_answer(&serve_with_plugin);
    finish_world();
}

int main(void) {
    a_typescript_only_loader_answers_one_layer_under_its_own_identity();
    a_loader_spanning_two_languages_answers_both_the_front_door_first();
    two_languages_on_one_runtime_answer_two_layers_each_from_its_own_item();
    the_first_required_plugin_puts_the_front_door_in_its_layer();
    a_loader_requiring_no_plugin_over_two_languages_is_refused_naming_both();
    a_language_the_catalog_hosts_in_no_item_is_refused_naming_it();
    a_language_host_on_jvm_is_refused_saying_no_jvm_start_exists();
    an_in_process_start_ends_its_layer_with_its_word();
    an_action_given_a_result_path_ends_its_start_with_it();
    an_in_process_start_of_two_languages_answers_the_front_doors_layer_alone();
    an_in_process_start_whose_front_doors_host_has_no_in_process_entry_is_refused();
    a_first_required_plugin_listed_in_no_language_is_refused_naming_it();
    an_action_of_a_fixture_plugin_answers_its_layer_alone();
    an_action_of_a_plugin_listed_in_no_language_is_refused_naming_it();
    a_loader_naming_no_catalog_reads_languages_from_spisor_hosts();
    a_loader_enabling_no_plugin_answers_no_layer();
    a_layer_with_no_hinted_version_is_refused_with_the_reinstall_fix();
    a_hint_naming_no_runtime_binary_is_refused_with_the_reinstall_fix();
    a_mode_or_its_members_out_of_shape_is_malformed();
    return check_summary("hosts_layer_start_test");
}
