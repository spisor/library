#ifndef RUNTIME_TEST_HOST_FIXTURE_H
#define RUNTIME_TEST_HOST_FIXTURE_H

#include <weivel/host.h>
#include <weivel/scope.h>

#include "directory_tree.h"
#include "holders.h"
#include "host_run.h"
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

/** A start's answer: its status, its plan and its refusal, released with release_run. */
typedef struct started_run {
    weivel_status status;
    weivel_standard_host_plan plan;
    weivel_standard_service_refusal refusal;
} started_run;

static inline started_run start_with(const char *const *arguments, size_t count) {
    const weivel_standard_host *host = host_definition();
    started_run run;
    memset(&run, 0, sizeof run);
    run.plan.size = sizeof run.plan;
    run.refusal.size = sizeof run.refusal;
    run.status = host->start(host->context, arguments, count, &run.plan, &run.refusal);
    return run;
}

static inline void release_run(started_run *run) {
    const weivel_standard_host *host = host_definition();
    host->release_plan(host->context, &run->plan);
    host->release_refusal(host->context, &run->refusal);
}

/** Stops the run, requiring the stop to answer OK with `expected` as the exit code. */
static inline void stop_expecting(uint32_t expected, const char *label) {
    const weivel_standard_host *host = host_definition();
    uint32_t exit_code = 999;
    weivel_status status = host->stop(host->context, &exit_code, NULL);
    if (status != WEIVEL_STATUS_OK || exit_code != expected) {
        fprintf(stderr, "%s: expected a stop with exit code %u, got status %d and exit code %u\n", label,
                (unsigned)expected, (int)status, (unsigned)exit_code);
        check_failures++;
    }
}

/** The path of the loader's `extension` file in the processes folder, as its holders and ready record are named. */
static inline char *processes_file(const char *extension) {
    char *problem = NULL;
    char *path = runtime_holders_path(state, LOADER, extension, &problem);
    spisor_memory_release(problem);
    return path;
}

/** Makes this process the loader's one holder, so a prune finds it held. */
static inline void hold_the_loader(void) {
    char record[160];
    char *path = processes_file(".json");
    snprintf(record, sizeof record, "{\"pid\":null,\"aliveAt\":null,\"checkMilliseconds\":null,\"holders\":[%d]}\n",
             (int)current_process_id());
    write_file(path, record);
    spisor_memory_release(path);
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
