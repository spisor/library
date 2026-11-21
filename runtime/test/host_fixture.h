#ifndef RUNTIME_TEST_HOST_FIXTURE_H
#define RUNTIME_TEST_HOST_FIXTURE_H

#include <weivel/host.h>
#include <weivel/scope.h>

#include "directory_tree.h"
#include "runtime/standard.h"
#include "updates_fixture.h"

#define LOADER "test-host"
#define PLUGIN_MANIFEST                                                                                            \
    "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"index.js\", \"config\": {\"name\": \"a\"}, "              \
    "\"actions\": {\"greet\": {\"call\": {\"service\": \"a.greeter\", \"method\": \"greet\", "                      \
    "\"argument\": {\"name\": \"world\", \"loud\": false}}}}}"

/** The places a host test runs in beside the updates fixture's cache and state, each absolute. */
static char user_home[1400];
static char application_home[1400];
static char portable[1400];
static char secrets[1400];
static char forebay_directory[1400];

static inline void set_variable(const char *name, const char *value) {
#if defined(_WIN32)
    CHECK(_putenv_s(name, value) == 0);
#else
    CHECK(setenv(name, value, 1) == 0);
#endif
}

/** This process's environment pointed at the places, its secrets in a file so no test reaches the OS store. */
static inline void point_the_environment_at_the_places(void) {
    set_variable("HOME", user_home);
    set_variable("USERPROFILE", user_home);
    set_variable("SPISOR_HOME", portable);
    set_variable("SPISOR_STATE", state);
    set_variable("SPISOR_CACHE", cache);
    set_variable("SPISOR_SECRETS", secrets);
}

static inline void write_layer_hint(void) {
    char hint[6000];
    int written = snprintf(hint, sizeof hint,
        "{\"host\": \"2.0.0\", \"libraries\": {"
        "\"runtime\": {\"version\": \"0.2.0\", \"directory\": \"%s/rt\", \"binary\": \"%s/rt/runtime.dll\"}, "
        "\"koffi\": {\"version\": \"2.0.0\", \"directory\": \"%s/koffi\"}, "
        "\"forebay-library\": {\"version\": \"0.3.0\", \"directory\": \"%s\", \"binary\": \"%s/forebay.dll\"}}}",
        scratch, scratch, scratch, forebay_directory, forebay_directory);
    CHECK(written > 0 && (size_t)written < sizeof hint);
    for (char *character = hint; *character != '\0'; character++) {
        if (*character == '\\') {
            *character = '/';
        }
    }
    write_hint("javascript-host", hint);
}

static inline void install_plugin(const char *id, const char *manifest) {
    char rest[512];
    mark_installed("bundles", id, "1.0.0");
    snprintf(rest, sizeof rest, "bundles/%s/1.0.0/plugin.json", id);
    write_in(cache, rest, manifest);
    snprintf(rest, sizeof rest, "bundles/%s/1.0.0/index.js", id);
    write_in(cache, rest, "export default {};\n");
}

/**
 * A registered loader whose settings enable plugin `a`, installed with PLUGIN_MANIFEST, the javascript-host's hint
 * naming runtime, koffi and forebay-library, and this process's environment pointed at it all.
 */
static inline void start_host_world(const char *label) {
    char registration[3200];
    start_base(label);
    snprintf(user_home, sizeof user_home, "%s/user", scratch);
    snprintf(application_home, sizeof application_home, "%s/application", scratch);
    snprintf(portable, sizeof portable, "%s/portable", scratch);
    snprintf(secrets, sizeof secrets, "%s/secrets", scratch);
    snprintf(forebay_directory, sizeof forebay_directory, "%s/forebay", scratch);
    write_catalog();
    install_plugin("a", PLUGIN_MANIFEST);
    snprintf(registration, sizeof registration,
             "{\"id\": \"" LOADER "\", \"kind\": \"host\", \"version\": \"1.0.0\", \"home\": \"%s\"}",
             application_home);
    for (char *character = registration; *character != '\0'; character++) {
        if (*character == '\\') {
            *character = '/';
        }
    }
    write_in(state, "hosts/" LOADER ".json", registration);
    cJSON *settings = settings_with("{\"plugins\": {\"a\": {\"enabled\": true}}}");
    char *text = cJSON_PrintUnformatted(settings);
    write_in(application_home, "config/settings.json", text);
    cJSON_free(text);
    cJSON_Delete(settings);
    write_layer_hint();
    point_the_environment_at_the_places();
}

static inline const weivel_standard_host *host_definition(void) {
    return runtime_standard_host_definition_of();
}

/** How many run records the loader has, which a start writes and its stop removes. */
static inline size_t run_record_count(void) {
    runtime_directory_listing listing = {NULL, 0};
    char *directory = joined(state, "bundles/runs/" LOADER);
    size_t count;
    CHECK(runtime_list_directory(directory, &listing) == SPISOR_OK);
    count = listing.count;
    runtime_free_directory_listing(&listing);
    spisor_memory_release(directory);
    return count;
}

#endif
