#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/allocation.h"
#include "definition_fixture.h"
#include "paths.h"
#include "runtime/paths.h"
#include "segment.h"
#include "cJSON.h"
#include "spisor/path.h"

#if defined(_WIN32)
#define TEST_HOME "C:\\h"
#define TEST_WINDOWS 1
#else
#define TEST_HOME "/h"
#define TEST_WINDOWS 0
#endif

static char *joined(const char *name) {
    const char *segments[2] = {TEST_HOME, name};
    return spisor_path_join(TEST_WINDOWS, segments, 2);
}

static void expect_joined(const char *actual, const char *name, const char *label) {
    char *expected = joined(name);
    CHECK(expected != NULL);
    if (expected != NULL) {
        check_text(actual, expected, label);
    }
    spisor_memory_release(expected);
}

static void expect_segment(const char *name, const char *expected) {
    char *segment;
    spisor_status status = runtime_portable_segment(name, &segment);
    CHECK(runtime_trims_to_portable_segment(name) == (expected != NULL));
    if (expected == NULL) {
        if (status != SPISOR_MALFORMED || segment != NULL) {
            fprintf(stderr, "segment \"%s\": expected MALFORMED, got %d\n", name, (int)status);
            check_failures++;
        }
    } else {
        CHECK(status == SPISOR_OK);
        check_text(segment, expected, name);
    }
    spisor_memory_release(segment);
}

static void segment_rules(void) {
    expect_segment(" repos ", "repos");
    expect_segment("", NULL);
    expect_segment("  ", NULL);
    expect_segment(".", NULL);
    expect_segment("..", NULL);
    expect_segment("a/b", NULL);
    expect_segment("a\\b", NULL);
    expect_segment("a:b", NULL);
    expect_segment("a.", NULL);
    expect_segment("a ", "a");
    expect_segment("con", NULL);
    expect_segment("COM1.txt", NULL);
    expect_segment("lpt9", NULL);
    expect_segment("console", "console");
    expect_segment("COM0", "COM0");
    expect_segment("COM10", "COM10");
    expect_segment("nul.txt", NULL);
    expect_segment("Aux", NULL);
    expect_segment("con.a.b", NULL);
}

static void segment_refuses_windows_forbidden_characters(void) {
    const char *forbidden[] = {"a<b", "a>b", "a\"b", "a|b", "a?b", "a*b", "a\x01" "b", "a\x1f" "b", "\x7f" "ok"};
    for (size_t index = 0; index < 8; index++) {
        expect_segment(forbidden[index], NULL);
    }
    expect_segment(forbidden[8], forbidden[8]);
    expect_segment("a\tb", NULL);
}

static void resolve_from(const char *names_json, runtime_paths *paths) {
    cJSON *names = names_json == NULL ? NULL : cJSON_Parse(names_json);
    CHECK(runtime_resolve_paths(TEST_HOME, names, paths) == 0);
    cJSON_Delete(names);
}

static void expect_kinds(const runtime_paths *paths, const char *repos, const char *plugin, const char *cache,
                         const char *config) {
    check_text(paths->home, TEST_HOME, "home");
    expect_joined(paths->repos, repos, "repos");
    expect_joined(paths->plugin, plugin, "plugin");
    expect_joined(paths->cache, cache, "cache");
    expect_joined(paths->config, config, "config");
}

static void defaults_when_absent(void) {
    runtime_paths paths;
    resolve_from(NULL, &paths);
    expect_kinds(&paths, "repos", "plugin", "cache", "config");
    runtime_free_paths(&paths);
}

static void declared_names_used(void) {
    runtime_paths paths;
    resolve_from("{\"repos\":\"r\",\"config\":\"settings\"}", &paths);
    expect_kinds(&paths, "r", "plugin", "cache", "settings");
    runtime_free_paths(&paths);
}

static void unusable_name_falls_back(void) {
    runtime_paths paths;
    resolve_from("{\"plugin\":\"../x\",\"cache\":5}", &paths);
    expect_kinds(&paths, "repos", "plugin", "cache", "config");
    runtime_free_paths(&paths);
}

static void duplicates_not_deduplicated(void) {
    runtime_paths paths;
    resolve_from("{\"repos\":\"config\"}", &paths);
    expect_kinds(&paths, "config", "plugin", "cache", "config");
    CHECK(strcmp(paths.repos, paths.config) == 0);
    runtime_free_paths(&paths);
}

#if defined(_WIN32)
#define TEST_PORTABLE "C:\\u\\.spisor"
#define TEST_CACHE "C:\\l\\spisor\\cache"
#else
#define TEST_PORTABLE "/u/.spisor"
#define TEST_CACHE "/u/.cache/spisor"
#endif

static const runtime_paths_definition *paths_definition(void) {
    return runtime_paths_definition_of();
}

static spisor_status resolve_with(const runtime_paths_resolve_argument *argument, runtime_paths_answer *answer,
                                  spisor_refusal *refusal) {
    const runtime_paths_definition *definition = paths_definition();
    return definition->resolve(definition->context, argument, answer, refusal);
}

static void release_both(runtime_paths_answer *answer, spisor_refusal *refusal) {
    const runtime_paths_definition *definition = paths_definition();
    definition->release_answer(definition->context, answer);
    definition->release_refusal(definition->context, refusal);
}

static void the_definition_is_one_static_struct(void) {
    const runtime_paths_definition *definition = paths_definition();
    CHECK(definition != NULL && (const void *)definition == runtime_paths_definition_of());
    CHECK(definition != NULL && definition->size == sizeof(runtime_paths_definition));
    CHECK(definition != NULL && definition->resolve != NULL && definition->release_answer != NULL &&
          definition->release_refusal != NULL);
}

static void resolves_every_folder_with_a_declared_name(void) {
    spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(value_of_text("{\"cache\":\"c\"}", &names));
    argument.home = TEST_HOME;
    argument.names = &names.root;
    CHECK(resolve_with(&argument, &answer, &refusal) == SPISOR_OK);
    check_text(answer.home, TEST_HOME, "home");
    expect_joined(answer.repos, "repos", "repos");
    expect_joined(answer.plugin, "plugin", "plugin");
    expect_joined(answer.cache, "c", "cache");
    expect_joined(answer.config, "config", "config");
    CHECK(answer.portable == NULL && answer.plugin_cache == NULL);
    CHECK(answer.storage != NULL && refusal.storage == NULL);
    release_both(&answer, &refusal);
    CHECK(answer.storage == NULL && answer.home == NULL && answer.size == sizeof answer);
    spisor_value_release(&names);
}

static void without_names_every_folder_takes_its_default(void) {
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    argument.home = TEST_HOME;
    CHECK(resolve_with(&argument, &answer, NULL) == SPISOR_OK);
    expect_joined(answer.cache, "cache", "cache");
    expect_joined(answer.config, "config", "config");
    release_both(&answer, NULL);
}

static void refuses(const runtime_paths_resolve_argument *argument, const char *detail_part, const char *label) {
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    check_refused(resolve_with(argument, &answer, &refusal), SPISOR_MALFORMED, &refusal, detail_part, label);
    CHECK(answer.storage == NULL && answer.home == NULL);
    release_both(&answer, &refusal);
    CHECK(refusal.storage == NULL && refusal.detail == NULL);
}

static void refuses_a_missing_or_unusable_home(void) {
    const char *homes[] = {NULL, "", "relative/home", "C:relative"};
    for (size_t index = 0; index < sizeof homes / sizeof homes[0]; index++) {
        runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
        argument.home = homes[index];
        refuses(&argument, "home", homes[index] == NULL ? "(no home)" : homes[index]);
    }
}

static void refuses_names_that_are_not_a_record(void) {
    spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    CHECK(value_of_text("[\"repos\"]", &names));
    argument.home = TEST_HOME;
    argument.names = &names.root;
    refuses(&argument, "names", "names as a list");
    spisor_value_release(&names);
}

static void refuses_an_argument_or_answer_short_of_its_shape(void) {
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.home = TEST_HOME;
    argument.size = (uint32_t)offsetof(runtime_paths_resolve_argument, places);
    refuses(&argument, "first shape", "a short argument");
    argument.size = sizeof argument;
    answer.size = (uint32_t)offsetof(runtime_paths_answer, storage);
    check_refused(resolve_with(&argument, &answer, &refusal), SPISOR_MALFORMED, &refusal, "first shape",
                  "a short answer");
    CHECK(answer.home == NULL);
    release_both(&answer, &refusal);
    refuses(NULL, "first shape", "no argument");
}

static void places_add_the_plugin_folders(void) {
    runtime_plugin_places places = SPISOR_INIT(runtime_plugin_places);
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    const char *portable_segments[3] = {TEST_PORTABLE, "plugins", "p"};
    const char *cache_segments[3] = {TEST_CACHE, "plugins", "p"};
    char *portable = spisor_path_join(TEST_WINDOWS, portable_segments, 3);
    char *cache = spisor_path_join(TEST_WINDOWS, cache_segments, 3);
    places.portable = TEST_PORTABLE;
    places.cache = TEST_CACHE;
    argument.home = TEST_HOME;
    argument.plugin = "p";
    argument.places = &places;
    CHECK(resolve_with(&argument, &answer, NULL) == SPISOR_OK);
    check_text(answer.portable, portable, "portable");
    check_text(answer.plugin_cache, cache, "plugin cache");
    expect_joined(answer.cache, "cache", "cache");
    release_both(&answer, NULL);
    spisor_memory_release(portable);
    spisor_memory_release(cache);
}

static void no_places_adds_nothing(void) {
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    argument.home = TEST_HOME;
    argument.plugin = "p";
    CHECK(resolve_with(&argument, &answer, NULL) == SPISOR_OK);
    CHECK(answer.home != NULL && answer.portable == NULL && answer.plugin_cache == NULL);
    release_both(&answer, NULL);
}

static void places_need_a_usable_plugin_and_places(void) {
    runtime_plugin_places usable = {sizeof(runtime_plugin_places), TEST_PORTABLE, TEST_CACHE};
    runtime_plugin_places relative = {sizeof(runtime_plugin_places), "relative", TEST_CACHE};
    runtime_plugin_places no_cache = {sizeof(runtime_plugin_places), TEST_PORTABLE, NULL};
    runtime_plugin_places short_of_shape = {(uint32_t)offsetof(runtime_plugin_places, cache), TEST_PORTABLE, TEST_CACHE};
    const runtime_plugin_places *places[] = {&usable, &usable, &relative, &no_cache, &short_of_shape};
    const char *plugins[] = {NULL, "../p", "p", "p", "p"};
    for (size_t index = 0; index < sizeof plugins / sizeof plugins[0]; index++) {
        runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
        argument.home = TEST_HOME;
        argument.plugin = plugins[index];
        argument.places = places[index];
        refuses(&argument, "places", "places case");
    }
}

int main(void) {
    segment_rules();
    segment_refuses_windows_forbidden_characters();
    defaults_when_absent();
    declared_names_used();
    unusable_name_falls_back();
    duplicates_not_deduplicated();
    the_definition_is_one_static_struct();
    resolves_every_folder_with_a_declared_name();
    without_names_every_folder_takes_its_default();
    refuses_a_missing_or_unusable_home();
    refuses_names_that_are_not_a_record();
    refuses_an_argument_or_answer_short_of_its_shape();
    places_add_the_plugin_folders();
    no_places_adds_nothing();
    places_need_a_usable_plugin_and_places();
    return check_summary("paths_test");
}
