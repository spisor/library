#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_deploy.h"
#include "catalog_call.h"
#include "check.h"
#include "service_scratch.h"
#include "tree_snapshot.h"

#define IDENTITY_ARGUMENT "{\"id\":\"github:5\",\"name\":\"greeter\"}"

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
    CHECK(spisor_make_directories(parent) == 1);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
    free(parent);
    free(path);
}

static char *read_from_scratch(const char *relative) {
    char *path = in_scratch(relative);
    size_t length = 0;
    char *text = (char *)spisor_read_file(path, &length);
    free(path);
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

static catalog_answer deploy_with(const char *identity_json) {
    char *directory = in_scratch("bundle");
    char *plugin_directory = in_scratch("plugins");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "directory", directory);
    cJSON_AddStringToObject(argument, "pluginDir", plugin_directory);
    cJSON_AddItemToObject(argument, "identity", parsed(identity_json));
    free(plugin_directory);
    free(directory);
    return call_catalog(runtime_cache_deploy, argument);
}

static void expect_file(const char *relative, const char *expected, const char *label) {
    char *text = read_from_scratch(relative);
    check_text(text, expected, label);
    free(text);
}

static void expect_nothing_written_beside_the_bundle(const char *before_snapshot, const char *label) {
    char *after = tree_snapshot(scratch, NULL);
    check_text(after, before_snapshot, label);
    free(after);
}

static void deploys_under_the_import_name(void) {
    start_case("deploys");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"dist/index.js\"}", "dist/index.js", "export default {};");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "importName"), "greeter", "answered import name");
    expect_file("plugins/greeter.json", "{\"importName\":\"greeter\",\"api\":1,\"entry\":\"dist/index.js\"}", "manifest");
    expect_file("plugins/greeter.js", "export default {};", "entry");
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    cJSON *identity = identity_text == NULL ? NULL : spisor_parse_json((const uint8_t *)identity_text, strlen(identity_text));
    CHECK(identity != NULL);
    check_text(text_at(identity, "id"), "github:5", "identity id");
    check_text(text_at(identity, "name"), "greeter", "identity name");
    cJSON_Delete(identity);
    free(identity_text);
    cJSON_Delete(answer.tree);
}

static void writes_no_entry_file_for_a_plugin_without_an_entry(void) {
    start_case("no-entry");
    write_bundle("{\"importName\":\"quiet\",\"api\":1}", NULL, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    char *entry = in_scratch("plugins/quiet.js");
    CHECK(!spisor_file_exists(entry));
    free(entry);
    char *manifest = in_scratch("plugins/quiet.json");
    CHECK(spisor_file_exists(manifest));
    free(manifest);
    cJSON_Delete(answer.tree);
}

static void writes_the_identity_from_its_argument_ignoring_a_stated_id(void) {
    start_case("stated-id");
    write_bundle("{\"importName\":\"greeter\",\"id\":\"github:999\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("bundle/greeter_identity.json", "{\"id\":\"github:999\",\"name\":\"stolen\"}");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    cJSON *identity = identity_text == NULL ? NULL : spisor_parse_json((const uint8_t *)identity_text, strlen(identity_text));
    check_text(text_at(identity, "id"), "github:5", "argument id wins");
    check_text(text_at(identity, "name"), "greeter", "argument name wins");
    cJSON_Delete(identity);
    free(identity_text);
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
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
        CHECK(expect_refusal(answer, "import name", refused[index]));
        expect_nothing_written_beside_the_bundle(before, refused[index]);
        free(before);
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
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
        CHECK(answer.status == SPISOR_STATUS_SUCCESS);
        check_text(text_at(answer.tree, "importName"), accepted[index], accepted[index]);
        cJSON_Delete(answer.tree);
    }
}

static void refuses_a_missing_import_name_and_writes_nothing(void) {
    start_case("no-name");
    write_bundle("{\"api\":1}", NULL, NULL);
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(expect_refusal(answer, "import name", "no import name"));
    expect_nothing_written_beside_the_bundle(before, "no import name");
    free(before);
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
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
        CHECK(expect_refusal(answer, "entry", entries[index]));
        expect_nothing_written_beside_the_bundle(before, entries[index]);
        free(before);
        cJSON_Delete(answer.tree);
    }
}

static void a_bundle_file_named_like_an_identity_file_is_never_copied(void) {
    start_case("identity-like");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("bundle/other_identity.json", "{\"id\":\"github:7\",\"name\":\"other\"}");
    write_in_scratch("bundle/dist/third_identity.json", "{\"id\":\"github:8\",\"name\":\"third\"}");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    char *copied = in_scratch("plugins/other_identity.json");
    CHECK(!spisor_file_exists(copied));
    free(copied);
    copied = in_scratch("plugins/third_identity.json");
    CHECK(!spisor_file_exists(copied));
    free(copied);
    char *identity_text = read_from_scratch("plugins/greeter_identity.json");
    CHECK(identity_text != NULL && strstr(identity_text, "github:7") == NULL);
    free(identity_text);
    cJSON_Delete(answer.tree);
}

static void refuses_an_identity_id_outside_the_grammar_and_writes_nothing(void) {
    const char *ids[] = {"calendar", "github:../x", "github:1/..", ""};
    for (size_t index = 0; index < sizeof ids / sizeof ids[0]; index++) {
        start_case("bad-identity");
        write_bundle("{\"importName\":\"greeter\",\"api\":1}", NULL, NULL);
        char *before = tree_snapshot(scratch, NULL);
        char identity[128];
        snprintf(identity, sizeof identity, "{\"id\":\"%s\",\"name\":\"greeter\"}", ids[index]);
        catalog_answer answer = deploy_with(identity);
        CHECK(answer.status == SPISOR_STATUS_FAILURE);
        expect_nothing_written_beside_the_bundle(before, ids[index]);
        free(before);
        cJSON_Delete(answer.tree);
    }
}

static void writes_the_module_marker_beside_the_plugins(void) {
    start_case("marker");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_file("plugins/package.json", "{\"type\":\"module\"}\n", "module marker");
    cJSON_Delete(answer.tree);
}

static void refuses_to_replace_another_plugin_and_changes_nothing(void) {
    start_case("replace");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer first = deploy_with(IDENTITY_ARGUMENT);
    CHECK(first.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(first.tree);
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer other = deploy_with("{\"id\":\"github:6\",\"name\":\"intruder\"}");
    CHECK(expect_refusal(other, "github:5", "names the recorded id"));
    CHECK(text_at(other.tree, "fix") != NULL && strstr(text_at(other.tree, "fix"), "github:6") != NULL);
    expect_nothing_written_beside_the_bundle(before, "nothing changed");
    free(before);
    cJSON_Delete(other.tree);
    catalog_answer again = deploy_with(IDENTITY_ARGUMENT);
    CHECK(again.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(again.tree);
}

static void treats_code_with_no_readable_identity_as_another_plugin(void) {
    start_case("orphan");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    write_in_scratch("plugins/greeter.js", "export default 'someone else';");
    char *before = tree_snapshot(scratch, NULL);
    catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(expect_refusal(answer, "greeter", "orphan code"));
    expect_nothing_written_beside_the_bundle(before, "orphan untouched");
    free(before);
    cJSON_Delete(answer.tree);
    write_in_scratch("plugins/greeter_identity.json", "not json");
    answer = deploy_with(IDENTITY_ARGUMENT);
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(answer.tree);
}

static void a_new_version_with_no_entry_removes_the_old_entry_file(void) {
    start_case("drop-entry");
    write_bundle("{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}", "index.js", "export default {};");
    catalog_answer first = deploy_with(IDENTITY_ARGUMENT);
    CHECK(first.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(first.tree);
    char *entry = in_scratch("plugins/greeter.js");
    CHECK(spisor_file_exists(entry));
    write_bundle("{\"importName\":\"greeter\",\"api\":1}", NULL, NULL);
    catalog_answer second = deploy_with(IDENTITY_ARGUMENT);
    CHECK(second.status == SPISOR_STATUS_SUCCESS);
    CHECK(!spisor_file_exists(entry));
    free(entry);
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
        catalog_answer answer = deploy_with(IDENTITY_ARGUMENT);
        CHECK(expect_refusal(answer, "entry", entries[index]));
        expect_nothing_written_beside_the_bundle(before, entries[index]);
        free(before);
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
        catalog_answer answer = call_catalog(runtime_cache_deploy, argument);
        CHECK(expect_refusal(answer, "pluginDir", directories[index]));
        expect_nothing_written_beside_the_bundle(before, directories[index]);
        free(plugins);
        free(bundle);
        free(before);
        cJSON_Delete(answer.tree);
    }
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
    refuses_an_identity_id_outside_the_grammar_and_writes_nothing();
    writes_the_module_marker_beside_the_plugins();
    refuses_to_replace_another_plugin_and_changes_nothing();
    treats_code_with_no_readable_identity_as_another_plugin();
    a_new_version_with_no_entry_removes_the_old_entry_file();
    refuses_an_entry_that_is_empty_or_not_text();
    refuses_a_plugin_directory_that_is_the_bundle_or_inside_it();
    return check_summary("cache_deploy_test");
}
