#ifndef RUNTIME_TEST_HOST_FIXTURE_H
#define RUNTIME_TEST_HOST_FIXTURE_H

#include <weivel/host.h>
#include <weivel/scope.h>

#include "directory_tree.h"
#include "holders.h"
#include "host_run.h"
#include "home_layout.h"
#include "home_prepare.h"
#include "home_write.h"
#include "places.h"
#include "runtime/standard.h"
#include "runtime_testing/home.h"
#include "spisor/json_value.h"
#include "updates_fixture.h"
#include "user_home.h"

#define LOADER "test-host"
#define LAYER_RUNTIME "javascript"
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

/** An empty entry module beside the scratch folder, so a home laid out in it starts empty; removed by the caller. */
static inline void write_entry_source(char *entry, size_t capacity) {
    snprintf(entry, capacity, "%s.index.js", scratch);
    write_file(entry, "export default {};\n");
}

/**
 * Installs plugin `id` with `manifest` again, as a plugin's next install would leave it: through runtime-testing's own
 * placing of one install, an empty module at the entry the manifest names, if it names one.
 */
static inline void install_plugin(const char *id, const char *manifest) {
    char entry[1200];
    cJSON *manifest_json = parsed(manifest);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    runtime_testing_install install = {&tree.root, NULL, NULL};
    runtime_testing_prepared_install prepared;
    runtime_testing_layout layout;
    CHECK(strcmp(text_at(manifest_json, "id"), id) == 0);
    CHECK(spisor_value_of_json(manifest_json, &tree) == SPISOR_OK);
    if (text_at(manifest_json, "entry") != NULL) {
        write_entry_source(entry, sizeof entry);
        install.entry_source = entry;
    }
    CHECK(runtime_testing_layout_of(scratch, &layout));
    CHECK(runtime_testing_prepare_install(&install, &layout, 0, &prepared, NULL) == SPISOR_OK);
    CHECK(runtime_testing_place_install(&layout, &prepared, NULL) == SPISOR_OK);
    runtime_testing_free_prepared_install(&prepared);
    runtime_testing_free_layout(&layout);
    if (install.entry_source != NULL) {
        CHECK(runtime_remove_tree(entry) == SPISOR_OK);
    }
    spisor_value_release(&tree);
    cJSON_Delete(manifest_json);
}

static inline void keep_variable(const runtime_variable *variable) {
    set_variable(variable->name, variable->value);
    if (strcmp(variable->name, RUNTIME_STATE_VARIABLE) == 0) {
        snprintf(state, sizeof state, "%s", variable->value);
    } else if (strcmp(variable->name, RUNTIME_CACHE_VARIABLE) == 0) {
        snprintf(cache, sizeof cache, "%s", variable->value);
    } else if (strcmp(variable->name, RUNTIME_PORTABLE_VARIABLE) == 0) {
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
    write_entry_source(entry, sizeof entry);
    snprintf(runtime_directory, sizeof runtime_directory, "%s/rt", scratch);
    snprintf(runtime_binary, sizeof runtime_binary, "%s/rt/runtime.dll", scratch);
    snprintf(forebay_directory, sizeof forebay_directory, "%s/forebay", scratch);
    snprintf(forebay_binary, sizeof forebay_binary, "%s/forebay.dll", forebay_directory);
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
 * naming runtime and forebay-library, and this process's environment pointed at it all.
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
