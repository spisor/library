#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_deploy.h"
#include "cache_deploy_hooks.h"
#include "spisor/allocation.h"
#include "cache_call.h"
#include "catalog_call.h"
#include "check.h"
#include "service_scratch.h"
#include "tree_snapshot.h"

#define IDENTITY_ARGUMENT "{\"id\":\"git:0000000000000000000000000000000000000005\",\"name\":\"greeter\"}"

static char scratch[1024];

static void start_case(const char *label) {
    scratch_in(scratch, sizeof scratch, "cache-deploy", label);
}

static char *in_scratch(const char *relative) {
    return joined_path(scratch, relative);
}

static void write_in_scratch(const char *relative, const char *text) {
    char *path = in_scratch(relative);
    char *parent = joined_path(path, "..");
    CHECK(spisor_make_directories(parent) == SPISOR_OK);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
    spisor_memory_release(parent);
    spisor_memory_release(path);
}

static char *read_from_scratch(const char *relative) {
    char *path = in_scratch(relative);
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_release(path);
    return text;
}

static void write_bundle(const char *manifest, const char *entry_name, const char *entry_text) {
    write_in_scratch("bundle/plugin.json", manifest);
    if (entry_name != NULL) {
        char relative[128];
        snprintf(relative, sizeof relative, "bundle/%s", entry_name);
        write_in_scratch(relative, entry_text);
    }
}

static catalog_answer deploy_with(const char *identity_json, const char *replaces) {
    char *directory = in_scratch("bundle");
    char *plugin_directory = in_scratch("plugins");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "directory", directory);
    cJSON_AddStringToObject(argument, "pluginDir", plugin_directory);
    cJSON_AddItemToObject(argument, "identity", parsed(identity_json));
    if (replaces != NULL) {
        cJSON_AddStringToObject(argument, "replaces", replaces);
    }
    spisor_memory_release(plugin_directory);
    spisor_memory_release(directory);
    return cache_call(CACHE_DEPLOY, argument);
}

static void expect_file(const char *relative, const char *expected, const char *label) {
    char *text = read_from_scratch(relative);
    check_text(text, expected, label);
    spisor_memory_release(text);
}

static void expect_nothing_written_beside_the_bundle(const char *before_snapshot, const char *label) {
    char *after = tree_snapshot(scratch, NULL);
    check_text(after, before_snapshot, label);
    spisor_memory_release(after);
}

static void deploys_under_the_import_name(void) {
    start_case("deploys");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"dist/index.js\"}", "dist/index.js", "export default {};");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_OK);
    check_text(text_at(answer.tree, "importName"), "greeter", "answered import name");
    expect_file("plugins/greeter.json", "{\"importName\":\"greeter\",\"api\":1,\"entry\":\"dist/index.js\"}", "manifest");
    expect_file("plugins/greeter.js", "export default {};", "entry");
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    cJSON *identity = identity_text == NULL ? NULL
                                            : spisor_parse_json_reporting((const uint8_t *)identity_text,
                                                                          strlen(identity_text), &(int){0});
    CHECK(identity != NULL);
    check_text(text_at(identity, "id"), "git:0000000000000000000000000000000000000005", "identity id");
    check_text(text_at(identity, "name"), "greeter", "identity name");
    cJSON_Delete(identity);
    spisor_memory_release(identity_text);
    cJSON_Delete(answer.tree);
}

static void writes_no_entry_file_for_a_plugin_without_an_entry(void) {
    start_case("no-entry");
    write_bundle("{\"importName\":\"quiet\",\"api\":1}", NULL, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_OK);
    char *entry = in_scratch("plugins/quiet.js");
    CHECK(spisor_probe_file(entry) != SPISOR_FILE_PRESENT);
    spisor_memory_release(entry);
    char *manifest = in_scratch("plugins/quiet.json");
    CHECK(spisor_probe_file(manifest) == SPISOR_FILE_PRESENT);
    spisor_memory_release(manifest);
    cJSON_Delete(answer.tree);
}

static void writes_the_identity_from_its_argument_ignoring_a_stated_id(void) {
    start_case("stated-id");
    write_bundle("{\"importName\":\"greeter\",\"id\":\"git:00000000000000000000000000000000000003e7\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("bundle/greeter_identity.json", "{\"id\":\"git:00000000000000000000000000000000000003e7\",\"name\":\"stolen\"}");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_OK);
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    cJSON *identity = identity_text == NULL ? NULL
                                            : spisor_parse_json_reporting((const uint8_t *)identity_text,
                                                                          strlen(identity_text), &(int){0});
    check_text(text_at(identity, "id"), "git:0000000000000000000000000000000000000005", "argument id wins");
    check_text(text_at(identity, "name"), "greeter", "argument name wins");
    cJSON_Delete(identity);
    spisor_memory_release(identity_text);
    cJSON_Delete(answer.tree);
}

static void refuses_an_import_name_that_fails_the_grammar_and_writes_nothing(void) {
    const char *refused[] = {"../x", "a/b", "A", "package", "a\\\\b", "con", "nul.x", "com1", "a..b", "-a", "a-", "", "x y"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        start_case("bad-name");
        char manifest[256];
        snprintf(manifest, sizeof manifest, "{\"importName\":\"%s\",\"api\":1,\"entry\":\"index.js\"}", refused[index]);
        write_bundle(manifest, "index.js", "export default {};");
        char *before = tree_snapshot(scratch, NULL);
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
        CHECK(expect_cache_refusal_refused(answer, "import name", refused[index]));
        expect_nothing_written_beside_the_bundle(before, refused[index]);
        spisor_memory_release(before);
        cJSON_Delete(answer.tree);
    }
}

static void accepts_the_import_names_the_grammar_allows(void) {
    const char *accepted[] = {"api.http", "config-ledger", "a1", "con-x", "com10", "x.y-z"};
    for (size_t index = 0; index < sizeof accepted / sizeof accepted[0]; index++) {
        start_case("good-name");
        char manifest[256];
        snprintf(manifest, sizeof manifest, "{\"importName\":\"%s\",\"api\":1}", accepted[index]);
        write_bundle(manifest, NULL, NULL);
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
        CHECK(answer.status == SPISOR_OK);
        check_text(text_at(answer.tree, "importName"), accepted[index], accepted[index]);
        cJSON_Delete(answer.tree);
    }
}

static void refuses_a_missing_import_name_and_writes_nothing(void) {
    start_case("no-name");
    write_bundle("{\"api\":1}", NULL, NULL);
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(expect_cache_refusal_refused(answer, "import name", "no import name"));
    expect_nothing_written_beside_the_bundle(before, "no import name");
    spisor_memory_release(before);
    cJSON_Delete(answer.tree);
}

static void refuses_an_entry_that_leaves_the_bundle_or_is_named_like_an_identity_file(void) {
    const char *entries[] = {"../outside.js", "other_identity.json", "OTHER_IDENTITY.JSON", "missing.js"};
    for (size_t index = 0; index < sizeof entries / sizeof entries[0]; index++) {
        start_case("bad-entry");
        char manifest[256];
        snprintf(manifest, sizeof manifest, "{\"importName\":\"greeter\",\"api\":1,\"entry\":\"%s\"}", entries[index]);
        write_bundle(manifest, "other_identity.json", "{}");
        write_in_scratch("outside.js", "export default {};");
        char *before = tree_snapshot(scratch, NULL);
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
        CHECK(expect_cache_refusal_refused(answer, "entry", entries[index]));
        expect_nothing_written_beside_the_bundle(before, entries[index]);
        spisor_memory_release(before);
        cJSON_Delete(answer.tree);
    }
}

static void a_bundle_file_named_like_an_identity_file_is_never_copied(void) {
    start_case("identity-like");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("bundle/other_identity.json", "{\"id\":\"git:0000000000000000000000000000000000000007\",\"name\":\"other\"}");
    write_in_scratch("bundle/dist/third_identity.json", "{\"id\":\"git:0000000000000000000000000000000000000008\",\"name\":\"third\"}");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_OK);
    char *copied = in_scratch("plugins/other_identity.json");
    CHECK(spisor_probe_file(copied) != SPISOR_FILE_PRESENT);
    spisor_memory_release(copied);
    copied = in_scratch("plugins/third_identity.json");
    CHECK(spisor_probe_file(copied) != SPISOR_FILE_PRESENT);
    spisor_memory_release(copied);
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    CHECK(identity_text != NULL && strstr(identity_text, "git:0000000000000000000000000000000000000007") == NULL);
    spisor_memory_release(identity_text);
    cJSON_Delete(answer.tree);
}

static void refuses_an_identity_id_outside_the_grammar_as_malformed_and_writes_nothing(void) {
    const char *ids[] = {"calendar", "git:../x", "git:0000000000000000000000000000000000000001/..", ""};
    for (size_t index = 0; index < sizeof ids / sizeof ids[0]; index++) {
        start_case("bad-identity");
        write_bundle("{\"importName\":\"greeter\",\"api\":1}", NULL, NULL);
        char *before = tree_snapshot(scratch, NULL);
        char identity[128];
        snprintf(identity, sizeof identity, "{\"id\":\"%s\",\"name\":\"greeter\"}", ids[index]);
        catalog_answer answer = deploy_with(identity, NULL);
        CHECK(answer.status == SPISOR_MALFORMED);
        expect_nothing_written_beside_the_bundle(before, ids[index]);
        spisor_memory_release(before);
        cJSON_Delete(answer.tree);
    }
}

static void writes_the_module_marker_beside_the_plugins(void) {
    start_case("marker");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_OK);
    expect_file("plugins/package.json", "{\"type\":\"module\"}\n", "module marker");
    cJSON_Delete(answer.tree);
}

static void refuses_to_replace_another_plugin_and_changes_nothing(void) {
    start_case("replace");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer first = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(first.status == SPISOR_OK);
    cJSON_Delete(first.tree);
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer other = deploy_with("{\"id\":\"git:0000000000000000000000000000000000000006\",\"name\":\"intruder\"}", NULL);
    CHECK(expect_cache_refusal_refused(other, "git:0000000000000000000000000000000000000005", "names the recorded id"));
    CHECK(text_at(other.tree, "fix") != NULL && strstr(text_at(other.tree, "fix"), "git:0000000000000000000000000000000000000006") != NULL);
    expect_nothing_written_beside_the_bundle(before, "nothing changed");
    spisor_memory_release(before);
    cJSON_Delete(other.tree);
    catalog_answer again = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(again.status == SPISOR_OK);
    cJSON_Delete(again.tree);
}

static void treats_code_with_no_readable_identity_as_another_plugin(void) {
    start_case("orphan");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("plugins/greeter.js", "export default 'someone else';");
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(expect_cache_refusal_refused(answer, "greeter", "orphan code"));
    expect_nothing_written_beside_the_bundle(before, "orphan untouched");
    spisor_memory_release(before);
    cJSON_Delete(answer.tree);
    write_in_scratch("plugins/greeter_identity.json", "not json");
    answer = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(answer.status == SPISOR_REFUSED);
    cJSON_Delete(answer.tree);
}

static void a_new_version_with_no_entry_removes_the_old_entry_file(void) {
    start_case("drop-entry");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer first = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(first.status == SPISOR_OK);
    cJSON_Delete(first.tree);
    char *entry = in_scratch("plugins/greeter.js");
    CHECK(spisor_probe_file(entry) == SPISOR_FILE_PRESENT);
    write_bundle("{\"importName\":\"greeter\",\"api\":1}", NULL, NULL);
    catalog_answer second = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(second.status == SPISOR_OK);
    CHECK(spisor_probe_file(entry) != SPISOR_FILE_PRESENT);
    spisor_memory_release(entry);
    cJSON_Delete(second.tree);
}

static void refuses_an_entry_that_is_empty_or_not_text(void) {
    const char *entries[] = {"\"\"", "5", "null", "[\"a\"]"};
    for (size_t index = 0; index < sizeof entries / sizeof entries[0]; index++) {
        start_case("odd-entry");
        char manifest[256];
        snprintf(manifest, sizeof manifest, "{\"importName\":\"greeter\",\"api\":1,\"entry\":%s}", entries[index]);
        write_bundle(manifest, NULL, NULL);
        char *before = tree_snapshot(scratch, NULL);
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT, NULL);
        CHECK(expect_cache_refusal_refused(answer, "entry", entries[index]));
        expect_nothing_written_beside_the_bundle(before, entries[index]);
        spisor_memory_release(before);
        cJSON_Delete(answer.tree);
    }
}

static void refuses_a_plugin_directory_that_is_the_bundle_or_inside_it(void) {
    const char *directories[] = {"bundle", "bundle/../bundle", "bundle/plugins"};
    for (size_t index = 0; index < sizeof directories / sizeof directories[0]; index++) {
        start_case("same-directory");
        write_bundle("{\"importName\":\"greeter\",\"api\":1}", NULL, NULL);
        char *before = tree_snapshot(scratch, NULL);
        char *bundle = in_scratch("bundle");
        char *plugins = in_scratch(directories[index]);
        cJSON *argument = cJSON_CreateObject();
        cJSON_AddStringToObject(argument, "directory", bundle);
        cJSON_AddStringToObject(argument, "pluginDir", plugins);
        cJSON_AddItemToObject(argument, "identity", parsed(IDENTITY_ARGUMENT));
        catalog_answer answer = cache_call(CACHE_DEPLOY, argument);
        CHECK(expect_cache_refusal_refused(answer, "pluginDir", directories[index]));
        expect_nothing_written_beside_the_bundle(before, directories[index]);
        spisor_memory_release(plugins);
        spisor_memory_release(bundle);
        spisor_memory_release(before);
        cJSON_Delete(answer.tree);
    }
}

static void replaces_the_plugin_of_the_id_it_is_told_to_and_no_other(void) {
    start_case("replaces");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer first = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(first.status == SPISOR_OK);
    cJSON_Delete(first.tree);
    catalog_answer moved = deploy_with("{\"id\":\"git:0000000000000000000000000000000000000006\",\"name\":\"moved\"}", "git:0000000000000000000000000000000000000005");
    CHECK(moved.status == SPISOR_OK);
    char *identity = read_from_scratch("plugins/greeter_identity.json");
    CHECK(identity != NULL && strstr(identity, "git:0000000000000000000000000000000000000006") != NULL && strstr(identity, "moved") != NULL);
    spisor_memory_release(identity);
    cJSON_Delete(moved.tree);
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer stale = deploy_with("{\"id\":\"git:0000000000000000000000000000000000000007\",\"name\":\"other\"}", "git:0000000000000000000000000000000000000005");
    CHECK(expect_cache_refusal_refused(stale, "git:0000000000000000000000000000000000000006", "the recorded id is no longer git:0000000000000000000000000000000000000005"));
    expect_nothing_written_beside_the_bundle(before, "stale replaces");
    spisor_memory_release(before);
    cJSON_Delete(stale.tree);
}

#define OTHER_IDENTITY "{\"id\":\"git:0000000000000000000000000000000000000009\",\"name\":\"other\"}"
#define LAST_MOVE 5

static void expect_deployed(catalog_answer answer, const char *label) {
    if (answer.status != SPISOR_OK) {
        fprintf(stderr, "%s: the deploy answered %d\n", label, (int)answer.status);
    }
    CHECK(answer.status == SPISOR_OK);
    cJSON_Delete(answer.tree);
}

/** Another plugin, then greeter's first version, deployed into this case's pluginDir. */
static void deploy_another_then_the_first_greeter(void) {
    write_bundle("{\"importName\":\"other\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default 'other';");
    expect_deployed(deploy_with(OTHER_IDENTITY, NULL), "the other plugin");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default 'one';");
    expect_deployed(deploy_with(IDENTITY_ARGUMENT, NULL), "the first greeter");
}

static void write_the_second_greeter(int with_entry) {
    if (with_entry) {
        write_bundle("{\"importName\":\"greeter\",\"api\":2,\"entry\":\"main.js\"}", "main.js", "export default 2;");
    } else {
        write_bundle("{\"importName\":\"greeter\",\"api\":2}", NULL, NULL);
    }
}

static char *plugin_directory_tree(void) {
    char *plugins = in_scratch("plugins");
    char *tree = tree_snapshot(plugins, NULL);
    spisor_memory_release(plugins);
    return tree;
}

/** pluginDir as a deploy of greeter's second version over its first leaves it when nothing stops it, freed. */
static char *cleanly_deployed_tree(int with_entry) {
    start_case(with_entry ? "clean-entry" : "clean-no-entry");
    deploy_another_then_the_first_greeter();
    write_the_second_greeter(with_entry);
    expect_deployed(deploy_with(IDENTITY_ARGUMENT, NULL), "the clean second greeter");
    return plugin_directory_tree();
}

/**
 * Stops the second version's deploy after each number of moves, as a crash would, then deploys it again: pluginDir
 * must end exactly as a clean deploy leaves it, the other plugin's files with it.
 */
static void finishes_a_deploy_stopped_after_any_move(int with_entry) {
    char *clean = cleanly_deployed_tree(with_entry);
    for (long moves = 0; moves <= LAST_MOVE; moves++) {
        char label[64];
        snprintf(label, sizeof label, "stopped-%s-after-%ld", with_entry ? "entry" : "no-entry", moves);
        start_case(label);
        deploy_another_then_the_first_greeter();
        write_the_second_greeter(with_entry);
        runtime_cache_stop_deploys_after(moves);
        catalog_answer stopped = deploy_with(IDENTITY_ARGUMENT, NULL);
        runtime_cache_stop_deploys_after(-1);
        CHECK(stopped.status != SPISOR_OK);
        cJSON_Delete(stopped.tree);
        expect_deployed(deploy_with(IDENTITY_ARGUMENT, NULL), label);
        char *tree = plugin_directory_tree();
        check_text(tree, clean, label);
        spisor_memory_release(tree);
    }
    spisor_memory_release(clean);
}

/** A deploy stopped part way through a first deploy of another identity is finished, and then refused as that one. */
static void finishes_a_stopped_deploy_of_another_identity_before_refusing_it(void) {
    start_case("stopped-other-identity");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default 'one';");
    runtime_cache_stop_deploys_after(2);
    catalog_answer stopped = deploy_with(OTHER_IDENTITY, NULL);
    runtime_cache_stop_deploys_after(-1);
    cJSON_Delete(stopped.tree);
    catalog_answer refused = deploy_with(IDENTITY_ARGUMENT, NULL);
    CHECK(expect_cache_refusal_refused(refused, "git:0000000000000000000000000000000000000009", "the finished deploy's identity"));
    cJSON_Delete(refused.tree);
    expect_file("plugins/greeter_identity.json", "{\n\t\"id\":\t\"git:0000000000000000000000000000000000000009\",\n\t\"name\":\t\"other\"\n}", "finished");
}

int main(void) {
    deploys_under_the_import_name();
    writes_no_entry_file_for_a_plugin_without_an_entry();
    writes_the_identity_from_its_argument_ignoring_a_stated_id();
    refuses_an_import_name_that_fails_the_grammar_and_writes_nothing();
    accepts_the_import_names_the_grammar_allows();
    refuses_a_missing_import_name_and_writes_nothing();
    refuses_an_entry_that_leaves_the_bundle_or_is_named_like_an_identity_file();
    a_bundle_file_named_like_an_identity_file_is_never_copied();
    refuses_an_identity_id_outside_the_grammar_as_malformed_and_writes_nothing();
    writes_the_module_marker_beside_the_plugins();
    refuses_to_replace_another_plugin_and_changes_nothing();
    treats_code_with_no_readable_identity_as_another_plugin();
    a_new_version_with_no_entry_removes_the_old_entry_file();
    refuses_an_entry_that_is_empty_or_not_text();
    refuses_a_plugin_directory_that_is_the_bundle_or_inside_it();
    replaces_the_plugin_of_the_id_it_is_told_to_and_no_other();
    finishes_a_deploy_stopped_after_any_move(1);
    finishes_a_deploy_stopped_after_any_move(0);
    finishes_a_stopped_deploy_of_another_identity_before_refusing_it();
    return check_summary("cache_deploy_test");
}
