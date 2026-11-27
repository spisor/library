#include "host_fixture.h"
#include "spisor/allocation.h"

#include "catalog.h"
#include "catalog_build.h"

static char base_folder[1024];

static char *in_base(const char *rest) {
    return joined(base_folder, rest);
}

static void start_case(const char *label) {
    snprintf(base_folder, sizeof base_folder, "%s/catalog-build-%d-%s", RUNTIME_SCRATCH_PATH, (int)fixture_process_id(), label);
    start_empty_directory(base_folder);
}

static void finish_case(void) {
    CHECK(runtime_remove_tree(base_folder) == SPISOR_OK);
}

/** A directory spisor-host bundle would write for plugin id at version, with extra manifest members. */
static char *bundled_plugin(const char *id, const char *version, const char *extra_members) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundled/%s/%s", id, version);
    char *directory = in_base(rest);
    char manifest[512];
    snprintf(manifest, sizeof manifest,
             "{\"importName\": \"%s\", \"version\": \"%s\", \"language\": \"typescript\", \"entry\": \"index.js\"%s}",
             id, version, extra_members == NULL ? "" : extra_members);
    write_in(directory, "plugin.json", manifest);
    write_in(directory, "index.js", "export default { activate() {} };\n");
    return directory;
}

static char *library_directory(void) {
    char *directory = in_base("library");
    write_in(directory, "package.json", "{\"version\": \"2.16.3\"}");
    write_in(directory, "lib/native.node", "native bytes");
    return directory;
}

/** The catalog at path as catalog.read reads it, origin its file url. */
static cJSON *read_back(const char *path) {
    char *text = text_in(path, "");
    char *origin = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, path, &origin);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "text", text == NULL ? "" : text);
    cJSON_AddStringToObject(argument, "origin", origin);
    catalog_answer read = catalog_service_call(CATALOG_READ, argument);
    if (read.status != SPISOR_OK) {
        char *printed = cJSON_PrintUnformatted(read.tree);
        fprintf(stderr, "the catalog was not read back: %s\n", printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON *catalog = cJSON_DetachItemFromObjectCaseSensitive(read.tree, "catalog");
    cJSON_Delete(read.tree);
    spisor_memory_release(origin);
    spisor_memory_release(text);
    return catalog;
}

static char *build(catalog_inputs *inputs, cJSON **refusal) {
    char *out = in_base("catalog");
    char *path = host_build_local_catalog(inputs, out, refusal);
    spisor_memory_release(out);
    return path;
}

static const cJSON *item_of(const cJSON *catalog, const char *id) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), id);
}

static void expect_refusal_naming(cJSON *refusal, const char *part, const char *label) {
    expect_contains(text_at(refusal, "message"), part, label);
    CHECK(text_at(refusal, "fix") != NULL);
    cJSON_Delete(refusal);
}

static int catalog_written(void) {
    return exists_in(base_folder, "catalog/catalog.json");
}

static void writes_an_index_of_relative_urls_the_reader_accepts_latest_by_semver(void) {
    start_case("latest");
    catalog_inputs inputs = {NULL, 0, 0};
    const char *versions[] = {"1.2.0", "1.10.0", "1.10.0-beta.1"};
    cJSON *refusal = NULL;
    for (size_t index = 0; index < 3; index++) {
        char *directory = bundled_plugin("greeter", versions[index], NULL);
        CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
        spisor_memory_release(directory);
    }
    char *path = build(&inputs, &refusal);
    char *expected = in_base("catalog/catalog.json");
    check_text(path, expected, "index path");
    cJSON *raw = json_in(base_folder, "catalog/catalog.json");
    const cJSON *reference = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item_of(raw, "greeter"), "versions"), "1.10.0"), "files"),
        "index.js");
    check_text(text_at(reference, "url"), "files/greeter/1.10.0/index.js", "relative url");
    cJSON *catalog = read_back(path);
    check_text(text_at(item_of(catalog, "greeter"), "latest"), "1.10.0", "latest by semver");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(item_of(catalog, "greeter"), "versions")) == 3);
    cJSON_Delete(catalog);
    cJSON_Delete(raw);
    spisor_memory_release(expected);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void lists_a_plugin_as_the_provider_of_the_install_method_its_manifest_names(void) {
    start_case("provider");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", ", \"installMethod\": \"fake\"");
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    check_text(text_at(item_of(catalog, "greeter"), "installMethod"), "fake", "install method");
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void keys_a_plugin_by_its_import_name_and_ignores_a_stated_id(void) {
    start_case("import-name");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", ", \"id\": \"someone-else\"");
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    CHECK(item_of(catalog, "greeter") != NULL);
    CHECK(item_of(catalog, "someone-else") == NULL);
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void refuses_a_plugin_manifest_that_names_an_id_and_no_import_name(void) {
    start_case("no-import-name");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = in_base("bundled/greeter/1.0.0");
    write_in(directory, "plugin.json", "{\"id\": \"greeter\", \"version\": \"1.0.0\", \"entry\": \"index.js\"}");
    write_in(directory, "index.js", "export default {};\n");
    CHECK(!host_read_plugin_directory(&inputs, directory, &refusal));
    expect_refusal_naming(refusal, "importName", "an id alone");
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void lists_every_copied_file_with_its_real_hash_and_size(void) {
    start_case("hashes");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", NULL);
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *raw = json_in(base_folder, "catalog/catalog.json");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item_of(raw, "greeter"), "versions"), "1.0.0"), "files");
    CHECK(cJSON_GetArraySize(files) == 3);
    const cJSON *file;
    cJSON_ArrayForEach(file, files) {
        char rest[512];
        snprintf(rest, sizeof rest, "catalog/%s", text_at(file, "url"));
        char *copied = in_base(rest);
        char hash[NAULA_SHA256_HEX_SIZE];
        uint64_t size = 0;
        CHECK(runtime_sha256_file_sized(copied, hash, &size) == SPISOR_OK);
        check_text(text_at(file, "sha256"), hash, "hash");
        CHECK((double)size == cJSON_GetObjectItemCaseSensitive(file, "size")->valuedouble);
        spisor_memory_release(copied);
    }
    cJSON_Delete(raw);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void places_a_per_platform_item_under_its_platform(void) {
    start_case("platform");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = library_directory();
    CHECK(host_read_library_directory(&inputs, "native-library", directory, "linux-x64", &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    const cJSON *item = item_of(catalog, "native-library");
    check_text(text_at(item, "kind"), "library", "kind");
    const cJSON *platforms = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item, "versions"), "2.16.3"), "platforms");
    const cJSON *reference = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(platforms, "linux-x64"), "files"), "lib/native.node");
    char *placed = in_base("catalog/files/native-library/2.16.3/linux-x64/lib/native.node");
    char *url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, placed, &url);
    check_text(text_at(reference, "url"), url, "resolved under its platform");
    spisor_memory_release(url);
    spisor_memory_release(placed);
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void installs_from_the_index_into_a_bundle_with_every_file(void) {
    start_case("installs");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", NULL);
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    char *cache = in_base("cache");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "id", "greeter");
    cJSON_AddItemToObject(argument, "item", cJSON_Duplicate(item_of(catalog, "greeter"), 1));
    cJSON_AddStringToObject(argument, "version", "1.0.0");
    catalog_answer installed = cache_call(CACHE_INSTALL, argument);
    CHECK(installed.status == SPISOR_OK);
    expect_json(cJSON_GetObjectItemCaseSensitive(installed.tree, "fetched"), "[\"index.js\",\"package.json\",\"plugin.json\"]",
                "every file fetched");
    CHECK(exists_in(cache, "bundles/greeter/1.0.0/index.js"));
    cJSON_Delete(installed.tree);
    spisor_memory_release(cache);
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

/** One input made by hand, as a bundler's output would be: kind, id, version, platform and files {key: source}. */
static void add_made_input(catalog_inputs *inputs, const char *kind, const char *id, const char *version, const char *platform,
                           const char *files_json) {
    if (inputs->count == inputs->capacity) {
        inputs->capacity = inputs->capacity == 0 ? 4 : inputs->capacity * 2;
        inputs->items = spisor_memory_reallocate(inputs->items, inputs->capacity * sizeof *inputs->items);
    }
    catalog_input *input = &inputs->items[inputs->count++];
    memset(input, 0, sizeof *input);
    input->kind = kind;
    input->id = copy_text(id);
    input->version = copy_text(version);
    input->platform = platform == NULL ? NULL : copy_text(platform);
    input->language = strcmp(kind, "plugin") == 0 ? copy_text("typescript") : NULL;
    input->files = parsed(files_json);
}

static char *source_file(void) {
    char *source = in_base("source/native.node");
    write_file(source, "native bytes");
    return source;
}

static char *files_with(const char *first_key, const char *second_key, const char *source) {
    cJSON *files = cJSON_CreateObject();
    cJSON_AddStringToObject(files, first_key, source);
    if (second_key != NULL) {
        cJSON_AddStringToObject(files, second_key, source);
    }
    char *text = cJSON_PrintUnformatted(files);
    cJSON_Delete(files);
    return text;
}

static void refuses_one_id_under_two_kinds_and_one_version_twice(void) {
    start_case("consistency");
    char *source = source_file();
    char *files = files_with("index.js", NULL, source);
    catalog_inputs kinds = {NULL, 0, 0};
    add_made_input(&kinds, "plugin", "greeter", "1.0.0", NULL, files);
    add_made_input(&kinds, "host", "greeter", "2.0.0", NULL, files);
    cJSON *refusal = NULL;
    CHECK(build(&kinds, &refusal) == NULL);
    expect_refusal_naming(refusal, "is given both as a plugin and as a host", "two kinds");
    catalog_inputs twice = {NULL, 0, 0};
    add_made_input(&twice, "plugin", "greeter", "1.0.0", NULL, files);
    add_made_input(&twice, "plugin", "greeter", "1.0.0", NULL, files);
    CHECK(build(&twice, &refusal) == NULL);
    expect_refusal_naming(refusal, "twice", "twice");
    CHECK(!catalog_written());
    host_free_catalog_inputs(&twice);
    host_free_catalog_inputs(&kinds);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_an_import_name_that_is_an_id_or_the_module_marker(void) {
    const char *refused[] = {"github:5", "local:x", "package", "con.x", "A"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; index++) {
        start_case("bad-import-name");
        catalog_inputs inputs = {NULL, 0, 0};
        cJSON *refusal = NULL;
        char *directory = in_base("bundled/odd/1.0.0");
        char manifest[256];
        snprintf(manifest, sizeof manifest,
                 "{\"importName\": \"%s\", \"version\": \"1.0.0\", \"language\": \"typescript\", \"entry\": \"index.js\"}",
                 refused[index]);
        write_in(directory, "plugin.json", manifest);
        write_in(directory, "index.js", "export default {};\n");
        CHECK(!host_read_plugin_directory(&inputs, directory, &refusal));
        expect_refusal_naming(refusal, "importName", refused[index]);
        spisor_memory_release(directory);
        host_free_catalog_inputs(&inputs);
        finish_case();
    }
}

static void keys_a_plugin_by_a_dotted_import_name(void) {
    start_case("dotted-name");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("api.http", "1.0.0", NULL);
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = path == NULL ? NULL : read_back(path);
    CHECK(item_of(catalog, "api.http") != NULL);
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void keys_an_item_by_an_id_as_well_as_by_a_name(void) {
    start_case("id-key");
    char *source = source_file();
    char *files = files_with("index.js", NULL, source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "plugin", "github:12", "1.0.0", NULL, files);
    add_made_input(&inputs, "plugin", "greeter", "1.0.0", NULL, files);
    cJSON *refusal = NULL;
    char *path = build(&inputs, &refusal);
    CHECK(path != NULL);
    cJSON *catalog = path == NULL ? NULL : read_back(path);
    CHECK(item_of(catalog, "github:12") != NULL);
    CHECK(item_of(catalog, "greeter") != NULL);
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_a_platform_that_is_not_an_os_arch_key_before_copying_anything(void) {
    start_case("bad-platform");
    char *source = source_file();
    char *files = files_with("lib/native.node", NULL, source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "library", "native-library", "2.16.3", "../escape", files);
    cJSON *refusal = NULL;
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, "platform", "platform");
    CHECK(!exists_in(base_folder, "escape") && !catalog_written());
    host_free_catalog_inputs(&inputs);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_a_file_the_installer_would_take_for_its_completion_marker_in_any_case(void) {
    start_case("marker");
    char *source = source_file();
    char *files = files_with(".INSTALLED.json", NULL, source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "library", "native-library", "2.16.3", "linux-x64", files);
    cJSON *refusal = NULL;
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, ".INSTALLED.json", "marker");
    CHECK(!catalog_written());
    host_free_catalog_inputs(&inputs);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_two_file_keys_that_differ_only_by_case_before_copying_anything(void) {
    start_case("folded");
    char *source = source_file();
    char *files = files_with("lib/Native.node", "lib/native.node", source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "library", "native-library", "2.16.3", "linux-x64", files);
    cJSON *refusal = NULL;
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, "lib/Native.node and lib/native.node", "both named");
    CHECK(!catalog_written());
    host_free_catalog_inputs(&inputs);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_a_platform_file_that_differs_only_by_case_from_a_file_given_for_every_platform(void) {
    start_case("folded-shared");
    char *source = source_file();
    char *shared = files_with("README.md", NULL, source);
    char *platform = files_with("readme.md", NULL, source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "library", "native-library", "2.16.3", NULL, shared);
    add_made_input(&inputs, "library", "native-library", "2.16.3", "linux-x64", platform);
    cJSON *refusal = NULL;
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, "differ only by case", "shared and platform");
    host_free_catalog_inputs(&inputs);
    cJSON_free(platform);
    cJSON_free(shared);
    spisor_memory_release(source);
    finish_case();
}

static void refuses_a_file_key_the_catalog_would_refuse(void) {
    start_case("bad-key");
    char *source = source_file();
    char *files = files_with("lib/../native.node", NULL, source);
    catalog_inputs inputs = {NULL, 0, 0};
    add_made_input(&inputs, "library", "native-library", "2.16.3", "linux-x64", files);
    cJSON *refusal = NULL;
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, "which a catalog refuses", "bad key");
    host_free_catalog_inputs(&inputs);
    cJSON_free(files);
    spisor_memory_release(source);
    finish_case();
}

#define STANDARD_MEMBERS                                                                                              \
    "\"application\": {\"id\": \"claude-code\", \"label\": \"Claude Code\", \"binary\": \"claude\", \"home\": "       \
    "{\"candidates\": [\"~/.claude\"]}, \"packages\": {\"npm\": \"@anthropic-ai/claude-code\"}}, \"pathCommands\": "  \
    "{\"requested\": [\"claude\"], \"minimum\": 0}, \"background\": [\"spisor-host\"], \"plugins\": []"

static char *loader_directory(const char *application_override) {
    char *directory = in_base("loader");
    char manifest[2048];
    snprintf(manifest, sizeof manifest, "{\"id\": \"claude-code-loader\", \"version\": \"1.0.0\", \"runtime\": \"native\", %s}",
             application_override == NULL ? STANDARD_MEMBERS : application_override);
    write_in(directory, "host.json", manifest);
    return directory;
}

static void lists_the_application_of_a_host_json_on_its_host_item_and_its_runtime(void) {
    start_case("loader");
    char *directory = loader_directory(NULL);
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(host_read_host_directory(&inputs, directory, NULL, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    const cJSON *item = item_of(catalog, "claude-code-loader");
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(item, "application"), "binary"), "claude", "application listed");
    check_text(text_at(item, "runtime"), "native", "runtime copied from host.json");
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void refuses_a_loader_directory_holding_anything_beside_its_host_json_naming_the_file(void) {
    start_case("loader-extra");
    char *directory = loader_directory(NULL);
    write_in(directory, "host.js", "export {};\n");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, directory, NULL, &refusal));
    expect_refusal_naming(refusal, "host.js", "names the file");
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void expect_marker_written(const char *rest, const char *label) {
    char *text = text_in(base_folder, rest);
    check_text(text, "{\"type\":\"module\"}\n", label);
    spisor_memory_release(text);
}

static void writes_the_esm_marker_beside_a_plugin_s_js_entry_itself(void) {
    start_case("plugin-marker");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", NULL);
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    CHECK(path != NULL);
    expect_marker_written("catalog/files/greeter/1.0.0/package.json", "plugin marker");
    cJSON *catalog = path == NULL ? NULL : read_back(path);
    const cJSON *files = value_at(item_of(catalog, "greeter"), "versions/1.0.0/files");
    CHECK(cJSON_HasObjectItem(files, "package.json") && cJSON_HasObjectItem(files, "index.js"));
    cJSON_Delete(catalog);
    cJSON_Delete(refusal);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void replaces_a_javascript_host_s_own_package_json_with_the_esm_marker(void) {
    start_case("host-marker");
    char *directory = in_base("scripted");
    write_in(directory, "host.json",
             "{\"id\": \"scripted-host\", \"version\": \"1.0.0\", \"runtime\": \"javascript\", \"entry\": \"lib/host.js\"}");
    write_in(directory, "lib/host.js", "export {};\n");
    write_in(directory, "lib/package.json", "{\"type\":\"commonjs\"}\n");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(host_read_host_directory(&inputs, directory, NULL, &refusal));
    char *path = build(&inputs, &refusal);
    CHECK(path != NULL);
    expect_marker_written("catalog/files/scripted-host/1.0.0/lib/package.json", "host marker replaces the bundle's");
    cJSON_Delete(refusal);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void refuses_an_entry_the_bundle_s_own_plugin_json_or_marker_would_replace(void) {
    const char *entries[] = {"plugin.json", "Package.json", "dist/package.json"};
    for (size_t index = 0; index < sizeof entries / sizeof entries[0]; index++) {
        start_case("replaced-entry");
        char *directory = in_base("replaced");
        char manifest[256];
        snprintf(manifest, sizeof manifest,
                 "{\"importName\": \"greeter\", \"version\": \"1.0.0\", \"language\": \"typescript\", \"entry\": \"%s\"}",
                 entries[index]);
        write_in(directory, "plugin.json", manifest);
        catalog_inputs inputs = {NULL, 0, 0};
        cJSON *refusal = NULL;
        CHECK(!host_read_plugin_directory(&inputs, directory, &refusal));
        expect_refusal_naming(refusal, "would replace", entries[index]);
        host_free_catalog_inputs(&inputs);
        spisor_memory_release(directory);
        finish_case();
    }
}

static void lists_no_application_for_a_host_json_that_has_none(void) {
    start_case("plain");
    char *directory = in_base("plain");
    write_in(directory, "host.json", "{\"id\": \"plain-host\", \"version\": \"1.0.0\", \"runtime\": \"javascript\", \"entry\": \"host.js\"}");
    write_in(directory, "host.js", "export {};\n");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(host_read_host_directory(&inputs, directory, NULL, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    CHECK(!cJSON_HasObjectItem(item_of(catalog, "plain-host"), "application"));
    check_text(text_at(item_of(catalog, "plain-host"), "runtime"), "javascript", "runtime");
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void refuses_an_application_on_anything_but_a_host(void) {
    start_case("application-plugin");
    char *directory = loader_directory(NULL);
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(host_read_host_directory(&inputs, directory, NULL, &refusal));
    inputs.items[0].kind = "plugin";
    CHECK(build(&inputs, &refusal) == NULL);
    expect_refusal_naming(refusal, "application", "application on a plugin");
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void refuses_a_host_json_whose_standard_is_invalid(void) {
    start_case("invalid-standard");
    char *directory = loader_directory(
        "\"application\": {\"id\": \"claude-code\", \"label\": \"Claude Code\", \"binary\": \"\", \"home\": {\"candidates\": "
        "[\"~/.claude\"]}, \"packages\": {}}, \"pathCommands\": {\"requested\": [], \"minimum\": 0}, \"background\": [], \"plugins\": []");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, directory, NULL, &refusal));
    expect_refusal_naming(refusal, "/application/binary", "names the path");
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void refuses_a_host_json_that_names_no_runtime(void) {
    start_case("no-runtime");
    char *directory = in_base("plain");
    write_in(directory, "host.json", "{\"id\": \"plain-host\", \"version\": \"1.0.0\", \"entry\": \"host.js\"}");
    write_in(directory, "host.js", "export {};\n");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, directory, NULL, &refusal));
    expect_refusal_naming(refusal, "names no runtime", "no runtime");
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void lists_a_plugin_in_the_language_its_manifest_names_and_no_runtime(void) {
    start_case("language");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", ", \"runtime\": \"javascript\"");
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    check_text(text_at(item_of(catalog, "greeter"), "language"), "typescript", "the manifest's language");
    CHECK(!cJSON_HasObjectItem(item_of(catalog, "greeter"), "runtime"));
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static void refuses_a_plugin_manifest_naming_no_language_or_one_outside_the_pattern(void) {
    const char *manifests[3] = {"{\"importName\": \"greeter\", \"version\": \"1.0.0\", \"entry\": \"index.js\"}",
                                "{\"importName\": \"greeter\", \"version\": \"1.0.0\", \"language\": \"Type Script\", "
                                "\"entry\": \"index.js\"}",
                                "{\"importName\": \"greeter\", \"version\": \"1.0.0\", \"language\": \"spisor\", "
                                "\"entry\": \"index.js\"}"};
    const char *named[3] = {"names no language", "names the language Type Script", "names the language spisor"};
    start_case("no-language");
    for (size_t index = 0; index < 3; index++) {
        char *directory = in_base("bundled/greeter/1.0.0");
        catalog_inputs inputs = {NULL, 0, 0};
        cJSON *refusal = NULL;
        write_in(directory, "plugin.json", manifests[index]);
        write_in(directory, "index.js", "export default {};\n");
        CHECK(!host_read_plugin_directory(&inputs, directory, &refusal));
        expect_contains(text_at(refusal, "fix"), "\"language\"", "the fix naming the member");
        expect_refusal_naming(refusal, named[index], named[index]);
        host_free_catalog_inputs(&inputs);
        spisor_memory_release(directory);
    }
    finish_case();
}

/** A host bundle directory under name holding host.json and the entry it names. */
static char *host_bundle(const char *name, const char *host_json, const char *entry) {
    char *directory = in_base(name);
    write_in(directory, "host.json", host_json);
    write_in(directory, entry, "built");
    return directory;
}

static void lists_a_language_host_and_an_interface_by_the_kind_their_host_json_names(void) {
    start_case("kinds");
    char *language_host = host_bundle("typescript-host",
                                      "{\"kind\": \"language-host\", \"id\": \"typescript-host\", \"version\": \"2.0.0\", "
                                      "\"language\": \"typescript\", \"runtime\": \"javascript\", \"entry\": \"host.mjs\"}",
                                      "host.mjs");
    char *renderer = host_bundle("interface",
                                  "{\"kind\": \"interface\", \"id\": \"spisor-interface\", \"version\": \"0.1.0\", "
                                  "\"runtime\": \"native\", \"entry\": \"spisor-interface\"}",
                                  "spisor-interface");
    char *host = host_bundle("host", "{\"id\": \"spisor-host\", \"version\": \"0.1.0\", \"runtime\": \"native\", "
                                     "\"entry\": \"spisor-host\"}",
                             "spisor-host");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(host_read_host_directory(&inputs, language_host, NULL, &refusal));
    CHECK(host_read_host_directory(&inputs, renderer, NULL, &refusal));
    CHECK(host_read_host_directory(&inputs, host, NULL, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *catalog = read_back(path);
    const cJSON *hosting = item_of(catalog, "typescript-host");
    check_text(text_at(hosting, "kind"), "language-host", "the language host's kind");
    check_text(text_at(hosting, "language"), "typescript", "the language it hosts");
    check_text(text_at(hosting, "runtime"), "javascript", "its runtime");
    check_text(text_at(item_of(catalog, "spisor-interface"), "kind"), "interface", "the interface's kind");
    CHECK(!cJSON_HasObjectItem(item_of(catalog, "spisor-interface"), "language"));
    check_text(text_at(item_of(catalog, "spisor-host"), "kind"), "host", "a host.json naming no kind");
    cJSON_Delete(catalog);
    spisor_memory_release(path);
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(host);
    spisor_memory_release(renderer);
    spisor_memory_release(language_host);
    finish_case();
}

static void refuses_a_language_host_not_named_for_its_language_and_an_interface_naming_one(void) {
    start_case("misnamed");
    char *misnamed = host_bundle("misnamed",
                                 "{\"kind\": \"language-host\", \"id\": \"rust-host\", \"version\": \"2.0.0\", "
                                 "\"language\": \"typescript\", \"runtime\": \"javascript\", \"entry\": \"host.mjs\"}",
                                 "host.mjs");
    char *unnamed = host_bundle("unnamed",
                                "{\"kind\": \"language-host\", \"id\": \"rust-host\", \"version\": \"2.0.0\", "
                                "\"runtime\": \"native\", \"entry\": \"weivel-host\"}",
                                "weivel-host");
    char *speaking = host_bundle("speaking",
                                 "{\"kind\": \"interface\", \"id\": \"spisor-interface\", \"version\": \"0.1.0\", "
                                 "\"language\": \"rust\", \"runtime\": \"native\", \"entry\": \"spisor-interface\"}",
                                 "spisor-interface");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, misnamed, NULL, &refusal));
    expect_refusal_naming(refusal, "hosts typescript, so its id must be typescript-host", "the misnamed language host");
    refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, unnamed, NULL, &refusal));
    expect_refusal_naming(refusal, "names no language", "a language host naming no language");
    refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, speaking, NULL, &refusal));
    expect_refusal_naming(refusal, "is an interface, which hosts no plugin and names no language", "the interface");
    CHECK(inputs.count == 0);
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(speaking);
    spisor_memory_release(unnamed);
    spisor_memory_release(misnamed);
    finish_case();
}

static void refuses_a_host_json_naming_an_unknown_kind(void) {
    start_case("unknown-kind");
    char *directory = host_bundle("odd", "{\"kind\": \"plugin\", \"id\": \"odd\", \"version\": \"1.0.0\", "
                                         "\"runtime\": \"native\", \"entry\": \"odd\"}",
                                  "odd");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    CHECK(!host_read_host_directory(&inputs, directory, NULL, &refusal));
    expect_refusal_naming(refusal, "names the kind plugin", "the kind no host directory is");
    host_free_catalog_inputs(&inputs);
    spisor_memory_release(directory);
    finish_case();
}

static void writes_the_catalog_format_runtime_reads(void) {
    start_case("format");
    catalog_inputs inputs = {NULL, 0, 0};
    cJSON *refusal = NULL;
    char *directory = bundled_plugin("greeter", "1.0.0", NULL);
    CHECK(host_read_plugin_directory(&inputs, directory, &refusal));
    char *path = build(&inputs, &refusal);
    cJSON *raw = json_in(base_folder, "catalog/catalog.json");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(raw, "format")) == RUNTIME_CATALOG_FORMAT);
    cJSON_Delete(raw);
    spisor_memory_release(path);
    spisor_memory_release(directory);
    host_free_catalog_inputs(&inputs);
    finish_case();
}

static int run_build(const char *const *arguments, program *run) {
    prepare_program(run, base_folder, "build");
    cJSON *list = strings_of(arguments);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "PATH");
    program_start(run, SPISOR_HOST_PATH, list, environment, base_folder, 1);
    cJSON_Delete(environment);
    cJSON_Delete(list);
    return program_finish(run, 60000);
}

static void builds_a_catalog_from_plugin_host_and_library_directories(void) {
    start_case("command");
    char *plugin = bundled_plugin("greeter", "1.0.0", NULL);
    char *host = in_base("host");
    write_in(host, "host.json", "{\"id\": \"spisor-host\", \"version\": \"0.1.0\", \"runtime\": \"native\", \"entry\": \"linux-x64/spisor-host\"}");
    write_in(host, "linux-x64/spisor-host", "native executable");
    char *library = library_directory();
    char library_argument[2048];
    snprintf(library_argument, sizeof library_argument, "native-library=%s@linux-x64", library);
    char *out = in_base("catalog");
    char *native_host = in_base("native-host");
    write_in(native_host, "host.json", "{\"id\": \"native-host\", \"version\": \"0.2.0\", \"runtime\": \"native\", \"entry\": \"native-host\"}");
    write_in(native_host, "native-host", "built for one platform");
    char native_argument[2048];
    snprintf(native_argument, sizeof native_argument, "%s@linux-x64", native_host);
    const char *arguments[] = {"catalog", "build", "--plugin", plugin, "--host", host, "--host", native_argument,
                               "--library", library_argument, "--out", out, NULL};
    program run;
    CHECK(run_build(arguments, &run) == 0);
    check_text(run.err, "", "nothing refused");
    expect_contains(run.out, "listing 4 builds", "what it wrote");
    char *index = joined(out, "catalog.json");
    cJSON *catalog = read_back(index);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    CHECK(cJSON_GetArraySize(items) == 4 && item_of(catalog, "greeter") != NULL && item_of(catalog, "spisor-host") != NULL &&
          item_of(catalog, "native-library") != NULL && item_of(catalog, "native-host") != NULL);
    const cJSON *host_files = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item_of(catalog, "spisor-host"), "versions"), "0.1.0"), "files");
    CHECK(cJSON_HasObjectItem(host_files, "host.json") && cJSON_HasObjectItem(host_files, "linux-x64/spisor-host"));
    const cJSON *native_platform = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(item_of(catalog, "native-host"), "versions"), "0.2.0"),
                                         "platforms"),
        "linux-x64");
    CHECK(cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(native_platform, "files"), "native-host"));
    cJSON_Delete(catalog);
    spisor_memory_release(index);
    program_free(&run);
    spisor_memory_release(native_host);
    spisor_memory_release(out);
    spisor_memory_release(library);
    spisor_memory_release(host);
    spisor_memory_release(plugin);
    finish_case();
}

static void answers_2_for_a_misused_catalog_command_line(void) {
    start_case("misuse");
    program unknown;
    const char *publish[] = {"publish", NULL};
    CHECK(run_build(publish, &unknown) == 2);
    expect_contains(unknown.err, "spisor-host catalog build", "the usage");
    program no_out;
    const char *without[] = {"catalog", "build", NULL};
    CHECK(run_build(without, &no_out) == 2);
    expect_contains(no_out.err, "--out", "out required");
    program library;
    const char *shapeless[] = {"catalog", "build", "--library", "native-library", "--out", "x", NULL};
    CHECK(run_build(shapeless, &library) == 2);
    expect_contains(library.err, "<id>=<directory>@<platform>", "library shape");
    program refused;
    char *empty = in_base("empty");
    CHECK(spisor_make_directories(empty) == SPISOR_OK);
    const char *nothing[] = {"catalog", "build", "--plugin", empty, "--out", "x", NULL};
    CHECK(run_build(nothing, &refused) == 1);
    expect_contains(refused.err, "has no plugin.json", "a refusal");
    expect_contains(refused.err, "fix: ", "its fix");
    spisor_memory_release(empty);
    program_free(&refused);
    program_free(&library);
    program_free(&no_out);
    program_free(&unknown);
    finish_case();
}

int main(void) {
    lists_a_plugin_in_the_language_its_manifest_names_and_no_runtime();
    refuses_a_plugin_manifest_naming_no_language_or_one_outside_the_pattern();
    lists_a_language_host_and_an_interface_by_the_kind_their_host_json_names();
    refuses_a_language_host_not_named_for_its_language_and_an_interface_naming_one();
    refuses_a_host_json_naming_an_unknown_kind();
    writes_the_catalog_format_runtime_reads();
    refuses_an_entry_the_bundle_s_own_plugin_json_or_marker_would_replace();
    writes_the_esm_marker_beside_a_plugin_s_js_entry_itself();
    replaces_a_javascript_host_s_own_package_json_with_the_esm_marker();
    writes_an_index_of_relative_urls_the_reader_accepts_latest_by_semver();
    lists_a_plugin_as_the_provider_of_the_install_method_its_manifest_names();
    keys_a_plugin_by_its_import_name_and_ignores_a_stated_id();
    refuses_a_plugin_manifest_that_names_an_id_and_no_import_name();
    lists_every_copied_file_with_its_real_hash_and_size();
    places_a_per_platform_item_under_its_platform();
    installs_from_the_index_into_a_bundle_with_every_file();
    refuses_one_id_under_two_kinds_and_one_version_twice();
    keys_an_item_by_an_id_as_well_as_by_a_name();
    refuses_an_import_name_that_is_an_id_or_the_module_marker();
    keys_a_plugin_by_a_dotted_import_name();
    refuses_a_platform_that_is_not_an_os_arch_key_before_copying_anything();
    refuses_a_file_the_installer_would_take_for_its_completion_marker_in_any_case();
    refuses_two_file_keys_that_differ_only_by_case_before_copying_anything();
    refuses_a_platform_file_that_differs_only_by_case_from_a_file_given_for_every_platform();
    refuses_a_file_key_the_catalog_would_refuse();
    lists_the_application_of_a_host_json_on_its_host_item_and_its_runtime();
    refuses_a_loader_directory_holding_anything_beside_its_host_json_naming_the_file();
    lists_no_application_for_a_host_json_that_has_none();
    refuses_an_application_on_anything_but_a_host();
    refuses_a_host_json_whose_standard_is_invalid();
    refuses_a_host_json_that_names_no_runtime();
    builds_a_catalog_from_plugin_host_and_library_directories();
    answers_2_for_a_misused_catalog_command_line();
    return check_summary("catalog_build_test");
}
