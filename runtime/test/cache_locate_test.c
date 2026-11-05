#include "updates_fixture.h"

#include "cache_marker.h"

typedef struct bundle {
    const char *manifest_version;
    const char *entry;
} bundle;

/** Installs id at version with a plugin.json naming bundle's version and entry, and a marker with hashes and sizes. */
static void installed(const char *id, const char *version, bundle shape) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s/%s", id, version);
    char *directory = joined(cache, rest);
    char manifest[512];
    snprintf(manifest, sizeof manifest, "{\"id\":\"%s\",\"api\":1,\"version\":\"%s\",\"entry\":\"index.js\"}", id,
             shape.manifest_version == NULL ? version : shape.manifest_version);
    const char *names[2] = {"plugin.json", shape.entry};
    const char *contents[2] = {manifest, "export default {};"};
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    for (size_t index = 0; index < 2 && names[index] != NULL; index++) {
        write_in(directory, names[index], contents[index]);
        char sha256[RUNTIME_SHA256_HEX_SIZE];
        sha256_of_text(contents[index], sha256);
        cJSON_AddStringToObject(hashes, names[index], sha256);
        cJSON_AddNumberToObject(sizes, names[index], (double)strlen(contents[index]));
    }
    CHECK(runtime_cache_write_marker(directory, hashes, sizes) == 1);
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    free(directory);
}

static catalog_answer located(const char *plugins_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddItemToObject(argument, "plugins", parsed(plugins_json));
    return call_catalog(runtime_cache_locate, argument);
}

static const cJSON *only(const cJSON *tree, const char *member, const char *label) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(tree, member);
    if (cJSON_GetArraySize(list) != 1) {
        fprintf(stderr, "%s: expected one %s, got %d\n", label, member, cJSON_GetArraySize(list));
        check_failures++;
        return NULL;
    }
    return list->child;
}

static void it_answers_no_entry_when_the_declared_entry_is_missing(void) {
    start_base("locate-no-entry");
    installed("a", "1.0.0", (bundle){NULL, NULL});
    catalog_answer answer = located("{\"a\":\"1.0.0\"}");
    const cJSON *found = only(answer.tree, "located", "no entry");
    check_text(text_at(found, "id"), "a", "no entry id");
    CHECK(found != NULL && !cJSON_HasObjectItem(found, "entry"));
    CHECK(text_at(found, "manifest") != NULL && strstr(text_at(found, "manifest"), "plugin.json") != NULL);
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_answers_the_entry_plugin_json_names_inside_the_version(void) {
    start_base("locate-entry");
    installed("a", "1.0.0", (bundle){NULL, "index.js"});
    catalog_answer answer = located("{\"a\":\"1.0.0\"}");
    const cJSON *found = only(answer.tree, "located", "entry");
    const char *entry = text_at(found, "entry");
    CHECK(entry != NULL && spisor_file_exists(entry) && strstr(entry, "index.js") != NULL);
    CHECK(text_at(found, "directory") != NULL && strstr(entry, text_at(found, "directory")) == entry);
    expect_member_json(answer.tree, "problems", "[]", "no problems");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_lists_every_id_it_was_given_that_is_installed_ordered_by_id(void) {
    start_base("locate-order");
    installed("b", "1.0.0", (bundle){NULL, "index.js"});
    installed("a", "1.0.0", (bundle){NULL, "index.js"});
    catalog_answer answer = located("{\"b\":\"1.0.0\",\"c\":\"1.0.0\",\"a\":\"1.0.0\"}");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(answer.tree, "located");
    CHECK(cJSON_GetArraySize(list) == 2);
    check_text(text_at(cJSON_GetArrayItem(list, 0), "id"), "a", "first");
    check_text(text_at(cJSON_GetArrayItem(list, 1), "id"), "b", "second");
    expect_member_json(answer.tree, "problems", "[]", "an id not installed is no problem");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_reports_a_version_whose_plugin_json_names_another_version(void) {
    start_base("locate-other-version");
    installed("a", "1.0.0", (bundle){"1.0.1", "index.js"});
    catalog_answer answer = located("{\"a\":\"1.0.0\"}");
    expect_member_json(answer.tree, "located", "[]", "not located");
    const cJSON *problem = only(answer.tree, "problems", "other version");
    check_text(text_at(problem, "id"), "a", "problem id");
    check_text(text_at(problem, "version"), "1.0.0", "problem version");
    check_text(text_at(problem, "file"), "plugin.json", "problem file");
    CHECK(text_at(problem, "detail") != NULL && strstr(text_at(problem, "detail"), "1.0.1") != NULL);
    CHECK(text_at(problem, "fix") != NULL);
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_reports_a_version_with_a_file_not_at_its_recorded_size_naming_the_file(void) {
    start_base("locate-damaged");
    installed("a", "1.0.0", (bundle){NULL, "index.js"});
    write_in(cache, "bundles/a/1.0.0/index.js", "export default {}; // longer");
    catalog_answer answer = located("{\"a\":\"1.0.0\"}");
    expect_member_json(answer.tree, "located", "[]", "damaged not located");
    const cJSON *problem = only(answer.tree, "problems", "damaged");
    check_text(text_at(problem, "file"), "index.js", "damaged file");
    CHECK(text_at(problem, "detail") != NULL && text_at(problem, "fix") != NULL);
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_locates_a_version_whose_files_kept_their_sizes_without_hashing_them(void) {
    start_base("locate-sizes");
    installed("a", "1.0.0", (bundle){NULL, "index.js"});
    write_in(cache, "bundles/a/1.0.0/index.js", "export default {}!");
    catalog_answer answer = located("{\"a\":\"1.0.0\"}");
    check_text(text_at(only(answer.tree, "located", "sizes"), "id"), "a", "located on sizes");
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_reports_an_id_that_would_name_a_folder_outside_the_bundles(void) {
    start_base("locate-outside");
    catalog_answer answer = located("{\"..\":\"1.0.0\"}");
    const cJSON *problem = only(answer.tree, "problems", "outside");
    CHECK(text_at(problem, "detail") != NULL && strstr(text_at(problem, "detail"), "no folder inside") != NULL);
    cJSON_Delete(answer.tree);
    finish_base();
}

static void it_refuses_an_argument_with_no_plugins_object(void) {
    start_base("locate-argument");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    catalog_answer answer = call_catalog(runtime_cache_locate, argument);
    expect_refusal(answer, "plugins", "no plugins");
    cJSON_Delete(answer.tree);
    finish_base();
}

int main(void) {
    it_answers_no_entry_when_the_declared_entry_is_missing();
    it_answers_the_entry_plugin_json_names_inside_the_version();
    it_lists_every_id_it_was_given_that_is_installed_ordered_by_id();
    it_reports_a_version_whose_plugin_json_names_another_version();
    it_reports_a_version_with_a_file_not_at_its_recorded_size_naming_the_file();
    it_locates_a_version_whose_files_kept_their_sizes_without_hashing_them();
    it_reports_an_id_that_would_name_a_folder_outside_the_bundles();
    it_refuses_an_argument_with_no_plugins_object();
    return check_summary("cache_locate_test");
}
