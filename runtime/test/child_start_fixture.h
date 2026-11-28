#ifndef RUNTIME_TEST_CHILD_START_FIXTURE_H
#define RUNTIME_TEST_CHILD_START_FIXTURE_H

#include "cache_marker.h"
#include "places.h"
#include "runtime/processes.h"
#include "sha256_file.h"
#include "updates_fixture.h"

/**
 * @file
 * A registered loader enabling plugin-stub, written in the fixture language `fixture-one`, whose language host is
 * native and runs action_fixture as its entry, so processes.run_action starts a real child that answers greet as
 * plugin-stub does.
 */

#define LOADER "test-loader"
#define LANGUAGE "fixture-one"
#define PLUGIN "plugin-stub"
#define LAYER_VERSION "2.0.0"
#define CHILD_ACTIONS "{\"greet\":{\"call\":{\"service\":\"compiled\",\"method\":\"greet\"}}}"

#if defined(_WIN32)
#define FIXTURE_ENTRY "action-fixture.exe"
#else
#define FIXTURE_ENTRY "action-fixture"
#endif

static char user_home[1400];
static char application_home[1400];
static char portable[1400];
static char runtime_binary[1400];
static char report_path[1400];
static char places_cache[1400];
static char places_state[1400];

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

static void install_plugin_stub(void) {
    list_plugin(PLUGIN, "1.0.0", "1.0.0");
    cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog_items, PLUGIN);
    cJSON_ReplaceItemInObjectCaseSensitive(item, "language", cJSON_CreateString(LANGUAGE));
    mark_installed("bundles", PLUGIN, "1.0.0");
    write_in(cache, "bundles/" PLUGIN "/1.0.0/plugin.json",
             "{\"id\":\"" PLUGIN "\",\"version\":\"1.0.0\",\"language\":\"" LANGUAGE
             "\",\"capabilities\":{\"provides\":[],\"consumes\":[]},\"actions\":" CHILD_ACTIONS "}");
}

/** Copies the file at source to directory/name and records its hash and size, as an install's marker does. */
static void copy_sized(const char *source, const char *directory, const char *name, cJSON *hashes, cJSON *sizes) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file_reporting(source, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    char *path = joined(directory, name);
    char sha256[NAULA_SHA256_HEX_SIZE];
    uint64_t size = 0;
    CHECK(bytes != NULL && spisor_make_directories(directory) == SPISOR_OK &&
          spisor_replace_file(path, bytes, length) == SPISOR_OK);
    CHECK(runtime_sha256_file_sized(path, sha256, &size) == SPISOR_OK);
    cJSON_AddStringToObject(hashes, name, sha256);
    cJSON_AddNumberToObject(sizes, name, (double)size);
    spisor_memory_release(path);
    spisor_memory_release(bytes);
}

/** Lists, installs and hints `fixture-one-host`, native, its entry a copy of the action fixture. */
static void install_language_host(void) {
    cJSON *item = cJSON_AddObjectToObject(catalog_items, LANGUAGE "-host");
    cJSON_AddStringToObject(item, "kind", "language-host");
    cJSON_AddStringToObject(item, "language", LANGUAGE);
    cJSON_AddStringToObject(item, "runtime", "native");
    cJSON_AddStringToObject(item, "latest", LAYER_VERSION);
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(item, "versions"), LAYER_VERSION), "files");
    char *directory = joined(cache, "bundles/" LANGUAGE "-host/" LAYER_VERSION);
    char *host_json = joined(directory, "host.json");
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    write_file(host_json, "{\"entry\":\"" FIXTURE_ENTRY "\"}");
    copy_sized(host_json, directory, "host.json", hashes, sizes);
    copy_sized(RUNTIME_ACTION_FIXTURE_PATH, directory, FIXTURE_ENTRY, hashes, sizes);
    CHECK(runtime_cache_write_marker(directory, hashes, sizes) == SPISOR_OK);
    cJSON *hint = cJSON_CreateObject();
    cJSON *library = cJSON_AddObjectToObject(cJSON_AddObjectToObject(hint, "libraries"), "runtime");
    cJSON_AddStringToObject(library, "version", "0.1.0");
    cJSON_AddStringToObject(library, "directory", scratch);
    cJSON_AddStringToObject(library, "binary", runtime_binary);
    cJSON_AddStringToObject(hint, "host", LAYER_VERSION);
    char *text = cJSON_PrintUnformatted(hint);
    write_hint(LANGUAGE "-host", text);
    cJSON_free(text);
    cJSON_Delete(hint);
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    spisor_memory_release(host_json);
    spisor_memory_release(directory);
}

static runtime_variable *environment_of(size_t *count) {
    static runtime_variable environment[7];
    environment[0] = (runtime_variable){"HOME", user_home};
    environment[1] = (runtime_variable){"USERPROFILE", user_home};
    environment[2] = (runtime_variable){"SPISOR_HOME", portable};
    environment[3] = (runtime_variable){"SPISOR_STATE", state};
    environment[4] = (runtime_variable){"SPISOR_CACHE", cache};
    environment[5] = (runtime_variable){"SPISOR_FIXTURE_REPORT", report_path};
    environment[6] = (runtime_variable){"SPISOR_FIXTURE_MARK", "the caller's environment"};
    *count = 7;
    return environment;
}

/** The cache and state the environment resolves to, as places' resolve answers them for the interface. */
static void resolve_places(void) {
    size_t count = 0;
    runtime_variable *variables = environment_of(&count);
    cJSON *environment = cJSON_CreateObject();
    cJSON *refusal = NULL;
    for (size_t index = 0; index < count; index++) {
        cJSON_AddStringToObject(environment, variables[index].name, variables[index].value);
    }
    cJSON *places = runtime_places_of_environment(environment, &refusal);
    CHECK(places != NULL);
    snprintf(places_cache, sizeof places_cache, "%s", text_at(places, "cache"));
    snprintf(places_state, sizeof places_state, "%s", text_at(places, "state"));
    cJSON_Delete(places);
    cJSON_Delete(refusal);
    cJSON_Delete(environment);
}

static void start_child_world(const char *label) {
    start_base(label);
    snprintf(user_home, sizeof user_home, "%s/user", scratch);
    snprintf(application_home, sizeof application_home, "%s/application", scratch);
    snprintf(portable, sizeof portable, "%s/portable", scratch);
    snprintf(runtime_binary, sizeof runtime_binary, "%s/runtime.dll", scratch);
    snprintf(report_path, sizeof report_path, "%s/report.json", scratch);
    write_file(runtime_binary, "runtime");
    register_loader();
    install_language_host();
    install_plugin_stub();
    write_tree(application_home, "config/settings.json", settings_with("{\"plugins\":{\"" PLUGIN "\":{\"enabled\":true}}}"));
    write_catalog();
    resolve_places();
}

/** How many entries stand in the state's results folder, 0 when it does not exist. */
static size_t results_left(void) {
    char *results = joined(state, "results");
    runtime_directory_listing listing;
    size_t count = 0;
    if (runtime_list_directory(results, &listing) == SPISOR_OK) {
        count = listing.count;
        runtime_free_directory_listing(&listing);
    }
    spisor_memory_release(results);
    return count;
}

static const runtime_processes_definition *processes(void) {
    return (const runtime_processes_definition *)runtime_processes_definition_of();
}

/** A run_action argument for plugin-stub's `action` with an empty input, in the world's places and environment. */
static runtime_processes_run_action_argument action_argument(const char *action) {
    runtime_processes_run_action_argument argument = SPISOR_INIT(runtime_processes_run_action_argument);
    argument.cache = places_cache;
    argument.state = places_state;
    argument.environment = environment_of(&argument.environment_count);
    argument.loader_id = LOADER;
    argument.plugin = PLUGIN;
    argument.action = action;
    argument.input = "{}";
    return argument;
}

#endif
