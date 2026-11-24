#ifndef RUNTIME_TEST_HOST_FIXTURE_H
#define RUNTIME_TEST_HOST_FIXTURE_H

#include <weivel/host.h>
#include <weivel/scope.h>

#include "directory_tree.h"
#include "holders.h"
#include "host_run.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "places.h"
#include "runtime/standard.h"
#include "runtime_testing/home.h"
#include "spisor/json_value.h"
#include "updates_fixture.h"
#include "user_home.h"

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

static inline const runtime_testing_definition *testing_definition(void) {
    return (const runtime_testing_definition *)runtime_testing_definition_of();
}

static inline void hash_bundle_file(const char *bundle, const char *name, cJSON *hashes, cJSON *sizes) {
    char hex[RUNTIME_SHA256_HEX_SIZE];
    uint64_t size = 0;
    char *path = joined(bundle, name);
    if (spisor_probe_file(path) == SPISOR_FILE_PRESENT && runtime_sha256_file_sized(path, hex, &size) == SPISOR_OK) {
        cJSON_AddStringToObject(hashes, name, hex);
        cJSON_AddNumberToObject(sizes, name, (double)size);
    }
    spisor_memory_release(path);
}

/** Installs plugin `id` at 1.0.0 with `manifest` and an empty index.js, marked complete as cache.install marks it. */
static inline void install_plugin(const char *id, const char *manifest) {
    int out_of_memory = 0;
    char *bundle = runtime_cache_bundle_directory(cache, id, "1.0.0", &out_of_memory);
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    char *manifest_path = joined(bundle, RUNTIME_PLUGIN_MANIFEST);
    char *entry_path = joined(bundle, "index.js");
    write_file(manifest_path, manifest);
    write_file(entry_path, "export default {};\n");
    hash_bundle_file(bundle, RUNTIME_PLUGIN_MANIFEST, hashes, sizes);
    hash_bundle_file(bundle, "index.js", hashes, sizes);
    CHECK(runtime_cache_write_marker(bundle, hashes, sizes) == SPISOR_OK);
    spisor_memory_release(entry_path);
    spisor_memory_release(manifest_path);
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    spisor_memory_release(bundle);
}

static inline void keep_variable(const runtime_variable *variable) {
    set_variable(variable->name, variable->value);
    if (strcmp(variable->name, RUNTIME_PORTABLE_VARIABLE) == 0) {
        snprintf(portable, sizeof portable, "%s", variable->value);
    } else if (strcmp(variable->name, RUNTIME_SECRETS_VARIABLE) == 0) {
        snprintf(secrets, sizeof secrets, "%s", variable->value);
    } else if (strcmp(variable->name, RUNTIME_HOME_VARIABLE) == 0) {
        snprintf(user_home, sizeof user_home, "%s", variable->value);
    }
}

/** Lays the world out at scratch through runtime-testing, plugin `a` installed with PLUGIN_MANIFEST. */
static inline void lay_out_host_world(int64_t holder) {
    char entry[1200];
    char runtime_directory[1200];
    char runtime_binary[1200];
    char forebay_binary[1500];
    runtime_boot_library libraries[2] = {{"runtime", "0.2.0", runtime_directory, runtime_binary},
                                         {"forebay-library", "0.3.0", forebay_directory, forebay_binary}};
    cJSON *manifest_json = parsed(PLUGIN_MANIFEST);
    spisor_value_tree manifest = SPISOR_INIT(spisor_value_tree);
    runtime_testing_install install = {&manifest.root, entry, NULL};
    runtime_testing_home_argument argument = SPISOR_INIT(runtime_testing_home_argument);
    runtime_testing_home_answer answer = SPISOR_INIT(runtime_testing_home_answer);
    snprintf(entry, sizeof entry, "%s.index.js", scratch);
    snprintf(runtime_directory, sizeof runtime_directory, "%s/rt", scratch);
    snprintf(runtime_binary, sizeof runtime_binary, "%s/rt/runtime.dll", scratch);
    snprintf(forebay_directory, sizeof forebay_directory, "%s/forebay", scratch);
    snprintf(forebay_binary, sizeof forebay_binary, "%s/forebay.dll", forebay_directory);
    write_file(entry, "export default {};\n");
    CHECK(spisor_value_of_json(manifest_json, &manifest) == SPISOR_OK);
    argument.base = scratch;
    argument.host_id = LOADER;
    argument.installs = &install;
    argument.install_count = 1;
    argument.libraries = libraries;
    argument.library_count = 2;
    argument.holder = holder;
    CHECK(testing_definition()->lay_out_home(NULL, &argument, &answer, NULL) == SPISOR_OK);
    for (size_t index = 0; index < answer.variable_count; index++) {
        keep_variable(&answer.variables[index]);
    }
    snprintf(application_home, sizeof application_home, "%s", answer.home == NULL ? "" : answer.home);
    testing_definition()->release_home_answer(NULL, &answer);
    spisor_value_release(&manifest);
    cJSON_Delete(manifest_json);
    CHECK(runtime_remove_tree(entry) == SPISOR_OK);
}

/**
 * A registered loader whose settings enable plugin `a`, installed with PLUGIN_MANIFEST, the javascript-host's hint
 * naming runtime, koffi and forebay-library, and this process's environment pointed at it all.
 */
static inline void start_host_world(const char *label) {
    start_base(label);
    lay_out_host_world(0);
}

/** start_host_world with this process recorded as the loader's one holder, so a prune finds it held. */
static inline void start_held_host_world(const char *label) {
    start_base(label);
    lay_out_host_world((int64_t)current_process_id());
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
    runtime_holders_record record = runtime_no_holders();
    char *path = processes_file(".json");
    CHECK(runtime_add_holder(&record, (double)current_process_id()));
    CHECK(runtime_write_holders(path, &record) == SPISOR_OK);
    runtime_free_holders(&record);
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
