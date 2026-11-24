#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

#include "boot_hint.h"
#include "cache_locate.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "catalog.h"
#include "catalog_call.h"
#include "check.h"
#include "config.h"
#include "holders.h"
#include "paths.h"
#include "registry.h"
#include "runtime/places.h"
#include "runtime_testing/home.h"
#include "scratch_directory.h"
#include "spisor/allocation.h"
#include "spisor/json_allocation.h"
#include "spisor/json_value.h"
#include "spisor/path.h"
#include "tree_snapshot.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define HOST "test-host"
#define MANIFEST "{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"dist/index.js\", \"config\": {\"name\": \"a\"}}"
#define ENTRY_TEXT "export default {};\n"

static char parent[1024];
static char base[1100];
static char entry_source[1100];
static char forebay_directory[1100];

static const runtime_testing_definition *testing(void) {
    return (const runtime_testing_definition *)runtime_testing_definition_of();
}

static char *joined(const char *left, const char *right) {
    const char *segments[2] = {left, right};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static void write_text(const char *path, const char *text) {
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
}

/** A parent folder holding an empty base, an entry source and a declared library beside it. */
static void start_parent(const char *label) {
    char declaration[1200];
    snprintf(parent, sizeof parent, "%s/runtime-testing-%d-%s", RUNTIME_SCRATCH_PATH, (int)current_process_id(), label);
    start_empty_directory(parent);
    snprintf(base, sizeof base, "%s/home", parent);
    CHECK(spisor_make_directories(base) == SPISOR_OK);
    snprintf(entry_source, sizeof entry_source, "%s/entry.js", parent);
    write_text(entry_source, ENTRY_TEXT);
    snprintf(forebay_directory, sizeof forebay_directory, "%s/forebay", parent);
    CHECK(spisor_make_directories(forebay_directory) == SPISOR_OK);
    snprintf(declaration, sizeof declaration, "%s/library.json", forebay_directory);
    write_text(declaration, "{\"importName\": \"forebay\", \"version\": \"0.3.0\", \"services\": []}");
}

static void finish_parent(void) {
    CHECK(runtime_remove_tree(parent) == SPISOR_OK);
}

/** Everything in the parent outside the base, so a call can be shown to have written nothing outside it. */
static char *outside_base(void) {
    return tree_snapshot(parent, "/home");
}

/** A value tree of json, which it deletes. */
static spisor_value_tree tree_of(cJSON *json) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    CHECK(spisor_value_of_json(json, &tree) == SPISOR_OK);
    cJSON_Delete(json);
    return tree;
}

typedef struct laid_out {
    spisor_status status;
    runtime_testing_home_answer answer;
    spisor_refusal refusal;
} laid_out;

static laid_out lay_out(const char *at, const runtime_testing_install *install, int64_t holder) {
    runtime_boot_library libraries[2] = {{"runtime", "0.2.0", NULL, NULL}, {"forebay-library", NULL, NULL, NULL}};
    runtime_testing_home_argument argument = SPISOR_INIT(runtime_testing_home_argument);
    laid_out result;
    char *runtime_directory = joined(parent, "rt");
    char *runtime_binary = joined(runtime_directory, "runtime.dll");
    libraries[0].directory = runtime_directory;
    libraries[0].binary = runtime_binary;
    libraries[1].directory = forebay_directory;
    argument.base = at;
    argument.host_id = HOST;
    argument.installs = install;
    argument.install_count = install == NULL ? 0 : 1;
    argument.libraries = libraries;
    argument.library_count = 2;
    argument.holder = holder;
    memset(&result, 0, sizeof result);
    result.answer.size = sizeof result.answer;
    result.refusal.size = sizeof result.refusal;
    result.status = testing()->lay_out_home(NULL, &argument, &result.answer, &result.refusal);
    spisor_memory_release(runtime_binary);
    spisor_memory_release(runtime_directory);
    return result;
}

static void release(laid_out *result) {
    testing()->release_home_answer(NULL, &result->answer);
    testing()->release_refusal(NULL, &result->refusal);
}

static const cJSON *first_of(const cJSON *tree, const char *member) {
    return cJSON_GetArrayItem(value_at(tree, member), 0);
}

static const char *variable(const runtime_testing_home_answer *answer, const char *name) {
    for (size_t index = 0; index < answer->variable_count; index++) {
        if (strcmp(answer->variables[index].name, name) == 0) {
            return answer->variables[index].value;
        }
    }
    return NULL;
}

static void expect_registered(const char *state) {
    cJSON *refusal = NULL;
    cJSON *registration = runtime_usable_registration(state, HOST, &refusal);
    CHECK(registration != NULL && refusal == NULL);
    cJSON_Delete(registration);
    cJSON_Delete(refusal);
}

static void expect_hinted(const char *cache) {
    int out_of_memory = 0;
    cJSON *hint = runtime_read_boot_hint(cache, "javascript-host", &out_of_memory);
    CHECK(hint != NULL);
    check_text(text_at(hint, "host"), "2.0.0", "hinted host");
    check_text(text_at(value_at(hint, "libraries/runtime"), "version"), "0.2.0", "runtime's version");
    check_text(text_at(value_at(hint, "libraries/forebay-library"), "version"), "0.3.0", "the declared version");
    const char *koffi = text_at(value_at(hint, "libraries/koffi"), "directory");
    CHECK(koffi != NULL && runtime_probe_entry(koffi) == RUNTIME_ENTRY_DIRECTORY);
    cJSON_Delete(hint);
}

static void expect_installed(const char *cache, const runtime_testing_placed *placed) {
    cJSON *check = runtime_cache_check_version(placed->bundle);
    cJSON *plugins = parsed("{\"a\": \"1.0.0\"}");
    cJSON *located = runtime_cache_locate_plugins(cache, plugins);
    check_text(text_at(check, "kind"), "intact", "the bundle's check");
    check_text(text_at(first_of(located, "located"), "id"), "a", "located plugin");
    CHECK(text_at(first_of(located, "located"), "entry") != NULL);
    cJSON_Delete(located);
    cJSON_Delete(plugins);
    cJSON_Delete(check);
}

static void expect_settings(const runtime_testing_home_answer *answer) {
    cJSON *refusal = NULL;
    cJSON *values = runtime_ecosystem_settings_values(answer->home, variable(answer, "SPISOR_HOME"), &refusal);
    cJSON *sources = runtime_catalog_sources(values);
    char *catalog = joined(base, "catalog/index.json");
    CHECK(cJSON_IsTrue(value_at(values, "plugins/a/enabled")));
    check_text(text_at(first_of(sources, "sources"), "kind"), "local", "the source's kind");
    char *listed = runtime_cache_resolve(text_at(first_of(sources, "sources"), "path"), NULL, 0);
    char *expected = runtime_cache_resolve(catalog, NULL, 0);
    check_text(listed, expected, "the source's path");
    spisor_memory_release(expected);
    spisor_memory_release(listed);
    spisor_memory_release(catalog);
    cJSON_Delete(sources);
    cJSON_Delete(values);
    cJSON_Delete(refusal);
}

static void expect_catalog_readable(void) {
    char *path = joined(base, "catalog/index.json");
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    cJSON *refusal = NULL;
    cJSON *read = text == NULL ? NULL : runtime_catalog_read(text, path, &refusal);
    CHECK(read != NULL && refusal == NULL);
    cJSON_Delete(read);
    cJSON_Delete(refusal);
    spisor_memory_release(text);
    spisor_memory_release(path);
}

static void expect_held(const runtime_testing_home_answer *answer) {
    char *problem = NULL;
    char *path = runtime_holders_path(variable(answer, "SPISOR_STATE"), HOST, ".json", &problem);
    runtime_holders_record record = runtime_no_holders();
    CHECK(path != NULL && runtime_read_holders(path, &record));
    CHECK(record.count == 1 && record.holders[0] == (double)current_process_id());
    runtime_free_holders(&record);
    spisor_memory_release(path);
    spisor_memory_release(problem);
}

static void expect_plugin_settings(const runtime_testing_home_answer *answer) {
    cJSON *config = parsed("{\"name\": \"a\"}");
    int failed = 0;
    cJSON *settings = runtime_read_settings(answer->home, NULL, variable(answer, "SPISOR_HOME"), "a", config, &failed);
    check_text(text_at(settings, "greeting"), "hi", "the plugin's stored setting");
    CHECK(answer->install_count == 1 && strstr(answer->installs[0].settings_file, "a.json") != NULL);
    cJSON_Delete(settings);
    cJSON_Delete(config);
}

static void a_laid_out_home_is_read_by_runtime(void) {
    start_parent("read");
    spisor_value_tree manifest = tree_of(parsed(MANIFEST));
    spisor_value_tree settings = tree_of(parsed("{\"greeting\": \"hi\"}"));
    runtime_testing_install install = {&manifest.root, entry_source, &settings.root};
    char *before = outside_base();
    laid_out result = lay_out(base, &install, current_process_id());
    char *after = outside_base();
    CHECK(result.status == SPISOR_OK);
    check_text(after, before, "the parent outside the base");
    if (result.status == SPISOR_OK) {
        CHECK(result.answer.variable_count == 6);
        expect_registered(variable(&result.answer, "SPISOR_STATE"));
        expect_hinted(variable(&result.answer, "SPISOR_CACHE"));
        expect_installed(variable(&result.answer, "SPISOR_CACHE"), &result.answer.installs[0]);
        expect_settings(&result.answer);
        expect_catalog_readable();
        expect_held(&result.answer);
        expect_plugin_settings(&result.answer);
    }
    release(&result);
    spisor_memory_release(after);
    spisor_memory_release(before);
    spisor_value_release(&settings);
    spisor_value_release(&manifest);
    finish_parent();
}

static void no_holder_writes_no_processes_record(void) {
    start_parent("unheld");
    laid_out result = lay_out(base, NULL, 0);
    CHECK(result.status == SPISOR_OK);
    if (result.status == SPISOR_OK) {
        runtime_directory_listing listing = {NULL, 0};
        CHECK(runtime_list_directory(result.answer.processes, &listing) == SPISOR_OK && listing.count == 0);
        runtime_free_directory_listing(&listing);
    }
    release(&result);
    finish_parent();
}

/** Lays out at `at` with the install given, requiring MALFORMED naming `fragment` and nothing written anywhere. */
static void expect_malformed(const char *at, const runtime_testing_install *install, const char *fragment,
                             const char *label) {
    char *before = tree_snapshot(parent, NULL);
    laid_out result = lay_out(at, install, current_process_id());
    char *after = tree_snapshot(parent, NULL);
    if (result.status != SPISOR_MALFORMED || result.refusal.detail == NULL ||
        strstr(result.refusal.detail, fragment) == NULL) {
        fprintf(stderr, "%s: expected MALFORMED naming %s, got %d: %s\n", label, fragment, (int)result.status,
                result.refusal.detail == NULL ? "(none)" : result.refusal.detail);
        check_failures++;
    }
    CHECK(result.answer.storage == NULL && result.answer.variable_count == 0);
    check_text(after, before, label);
    release(&result);
    spisor_memory_release(after);
    spisor_memory_release(before);
}

static void a_base_that_cannot_hold_a_home_is_refused(void) {
    char *missing;
    char *occupied;
    start_parent("refused-base");
    missing = joined(parent, "missing");
    occupied = joined(parent, "forebay");
    expect_malformed("relative/home", NULL, "absolute", "a relative base");
    expect_malformed(missing, NULL, "no folder", "a missing base");
    expect_malformed(occupied, NULL, "not empty", "a base that is not empty");
    spisor_memory_release(occupied);
    spisor_memory_release(missing);
    finish_parent();
}

static void an_entry_climbing_out_of_its_bundle_is_refused(void) {
    start_parent("escaping-entry");
    spisor_value_tree manifest =
        tree_of(parsed("{\"id\": \"a\", \"version\": \"1.0.0\", \"entry\": \"../../../../escaped.js\"}"));
    runtime_testing_install install = {&manifest.root, entry_source, NULL};
    expect_malformed(base, &install, "climbs out", "an escaping entry");
    spisor_value_release(&manifest);
    finish_parent();
}

static int is_same_or_inside(const char *place, const char *path) {
    char *resolved_place = runtime_cache_resolve(place, NULL, 0);
    char *resolved_path = runtime_cache_resolve(path, NULL, 0);
    int covered = resolved_place != NULL && resolved_path != NULL &&
                  (strcmp(resolved_place, resolved_path) == 0 ||
                   runtime_cache_is_strictly_inside(resolved_path, resolved_place));
    spisor_memory_release(resolved_path);
    spisor_memory_release(resolved_place);
    return covered;
}

/** Whether every place `places` resolves lies at or inside one of `paths`, and every path is one of the places. */
static void expect_places_covered(const runtime_places_answer *places, const runtime_testing_real_places_answer *real) {
    const char *const found[4] = {places->portable, places->state, places->cache, places->secrets_file};
    for (size_t index = 0; index < 4; index++) {
        int covered = 0;
        for (size_t path = 0; path < real->path_count; path++) {
            covered = covered || is_same_or_inside(found[index], real->paths[path]);
        }
        CHECK(covered);
    }
    for (size_t path = 0; path < real->path_count; path++) {
        int named = 0;
        for (size_t index = 0; index < 4; index++) {
            char *resolved = runtime_cache_resolve(found[index], NULL, 0);
            named = named || (resolved != NULL && strcmp(resolved, real->paths[path]) == 0);
            spisor_memory_release(resolved);
        }
        CHECK(named);
        for (size_t other = 0; other < real->path_count; other++) {
            CHECK(other == path || !runtime_cache_is_strictly_inside(real->paths[other], real->paths[path]));
        }
    }
}

static void expect_real_places_of(const runtime_variable *environment, size_t count, size_t expected_count) {
    const runtime_places_definition *places = (const runtime_places_definition *)runtime_places_definition_of();
    runtime_places_resolve_argument resolve = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer resolved = SPISOR_INIT(runtime_places_answer);
    runtime_testing_real_places_argument argument = SPISOR_INIT(runtime_testing_real_places_argument);
    runtime_testing_real_places_answer real = SPISOR_INIT(runtime_testing_real_places_answer);
    resolve.environment = environment;
    resolve.environment_count = count;
    argument.environment = environment;
    argument.environment_count = count;
    CHECK(places->resolve(places->context, &resolve, &resolved, NULL) == SPISOR_OK);
    CHECK(testing()->real_places(NULL, &argument, &real, NULL) == SPISOR_OK);
    CHECK(real.path_count == expected_count);
    expect_places_covered(&resolved, &real);
    testing()->release_real_places_answer(NULL, &real);
    places->release_answer(places->context, &resolved);
}

static void real_places_are_the_places_resolve_answers(void) {
    char user[1200];
    char local[1200];
    char secrets[1200];
    start_parent("real-places");
    snprintf(user, sizeof user, "%s/user", parent);
    snprintf(local, sizeof local, "%s/local", parent);
    snprintf(secrets, sizeof secrets, "%s/secrets", parent);
    runtime_variable environment[4] = {
        {"HOME", user}, {"USERPROFILE", user}, {"LOCALAPPDATA", local}, {"SPISOR_SECRETS", secrets}};
    expect_real_places_of(environment, 3, RUNTIME_HOST_IS_WINDOWS ? 2 : 3);
    expect_real_places_of(environment, 4, RUNTIME_HOST_IS_WINDOWS ? 3 : 4);
    finish_parent();
}

int main(void) {
    spisor_json_use_library_allocator();
    a_laid_out_home_is_read_by_runtime();
    no_holder_writes_no_processes_record();
    a_base_that_cannot_hold_a_home_is_refused();
    an_entry_climbing_out_of_its_bundle_is_refused();
    real_places_are_the_places_resolve_answers();
    return check_summary("runtime_testing_test");
}
