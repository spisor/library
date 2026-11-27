#include "host_fixture.h"
#include "spisor/allocation.h"

#include "links.h"
#include "on_path.h"
#include "process_environment.h"
#include "tree_snapshot.h"

#define SHA256 "e2edda75bfb089654705cf65a13b58933bfd5f57148e8ebda916f58a2474f1f7"
#define OTHER_TRIPLET "freebsd_ia32"

static char base_folder[1024];

static char *in_base(const char *rest) {
    return joined(base_folder, rest);
}

static void start_case(const char *label) {
    snprintf(base_folder, sizeof base_folder, "%s/stage-bundle-%d-%s", RUNTIME_SCRATCH_PATH, (int)fixture_process_id(),
             label);
    start_empty_directory(base_folder);
}

static void finish_case(void) {
    CHECK(runtime_remove_tree(base_folder) == SPISOR_OK);
}

/** The name koffi's prebuilt folder carries for this platform, as koffi's own loader names it. */
static void koffi_triplet(char *triplet, size_t size) {
    snprintf(triplet, size, "%s", runtime_platform_key());
    for (char *at = triplet; *at != 0; at++) {
        *at = *at == '-' ? '_' : *at;
    }
}

/** The file name an entry looks for library under beside itself on this platform. */
static void shared_library_name(const char *library, char *name, size_t size) {
#if defined(_WIN32)
    snprintf(name, size, "%s.dll", library);
#elif defined(__APPLE__)
    snprintf(name, size, "lib%s.dylib", library);
#else
    snprintf(name, size, "lib%s.so", library);
#endif
}

/** `<platform>/<rest>`, the key a file staged per platform lands under. */
static void per_platform(const char *rest, char *key, size_t size) {
    snprintf(key, size, "%s/%s", runtime_platform_key(), rest);
}

/** Runs the program with arguments in the working directory, PATH and SystemRoot passed on; its exit code. */
static int run_in(const char *executable, const char *const *arguments, const char *directory, program *run) {
    prepare_program(run, base_folder, "stage");
    cJSON *list = strings_of(arguments);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "PATH");
    program_start(run, executable, list, environment, directory, 1);
    cJSON_Delete(environment);
    cJSON_Delete(list);
    return program_finish(run, 120000);
}

static int stage(const char *const *arguments, program *run) {
    return run_in(STAGE_BUNDLE_PATH, arguments, base_folder, run);
}

static void expect_file(const char *directory, const char *rest, const char *content) {
    char *text = text_in(directory, rest);
    check_text(text, content, rest);
    spisor_memory_release(text);
}

static void expect_manifest(const char *directory, const char *expected_text) {
    cJSON *manifest = json_in(directory, "host.json");
    cJSON *expected = parsed(expected_text);
    if (manifest == NULL || !cJSON_Compare(manifest, expected, 1)) {
        char *printed = manifest == NULL ? NULL : cJSON_PrintUnformatted(manifest);
        fprintf(stderr, "host.json: %s\n", printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
    cJSON_Delete(manifest);
}

/** The core and standard as built files under <base>/built. */
static void write_libraries(void) {
    write_in(base_folder, "built/core.bin", "the core");
    write_in(base_folder, "built/standard.bin", "the standard");
}

/**
 * A native language host's inputs in <base>/inputs: its entry `weivel-host` and a directory `data` of two files, and
 * the host-data file holding host_data.
 */
static void write_native_inputs(const char *host_data) {
    write_libraries();
    write_in(base_folder, "inputs/weivel-host", "the host program");
    write_in(base_folder, "inputs/data/one.txt", "one");
    write_in(base_folder, "inputs/data/nested/two.txt", "two");
    write_in(base_folder, "host-data.json", host_data);
}

/** Stages fixture-one's native host into <base>/bundle at 3.2.1 from <base>/inputs; its exit code. */
static int stage_native_host(const char *language, const char *runtime, program *run) {
    char *directory = in_base("bundle");
    char *data = in_base("host-data.json");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    char *inputs = in_base("inputs");
    const char *arguments[] = {"language-host", directory, "3.2.1", language, runtime, "weivel-host", data, core,
                               standard, "data", NULL};
    int code = run_in(STAGE_BUNDLE_PATH, arguments, inputs, run);
    spisor_memory_release(inputs);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(data);
    spisor_memory_release(directory);
    return code;
}

static void stages_a_native_language_host_under_its_platform_with_the_host_data_merged(void) {
    start_case("native");
    write_native_inputs("{\"flavour\": {\"sweet\": true}}");
    program run;
    CHECK(stage_native_host("fixture-one", "native", &run) == 0);
    check_text(run.err, "", "nothing refused");
    char *directory = in_base("bundle");
    char key[256];
    char library[64];
    per_platform("weivel-host", key, sizeof key);
    expect_file(directory, key, "the host program");
    per_platform("data/one.txt", key, sizeof key);
    expect_file(directory, key, "one");
    per_platform("data/nested/two.txt", key, sizeof key);
    expect_file(directory, key, "two");
    shared_library_name("weivel_core", library, sizeof library);
    per_platform(library, key, sizeof key);
    expect_file(directory, key, "the core");
    shared_library_name("weivel_standard", library, sizeof library);
    per_platform(library, key, sizeof key);
    expect_file(directory, key, "the standard");
    CHECK(!exists_in(directory, "weivel-host") && !exists_in(directory, "data"));
    char expected[1024];
    snprintf(expected, sizeof expected,
             "{\"kind\": \"language-host\", \"id\": \"fixture-one-host\", \"version\": \"3.2.1\", "
             "\"language\": \"fixture-one\", \"runtime\": \"native\", \"entry\": \"%s/weivel-host\", "
             "\"flavour\": {\"sweet\": true}}",
             runtime_platform_key());
    expect_manifest(directory, expected);
    spisor_memory_release(directory);
    program_free(&run);
    finish_case();
}

/** stage-bundle refuses the native host staged with host_data, language and runtime, naming part, staging nothing. */
static void expect_native_host_refused(const char *label, const char *host_data, const char *language,
                                       const char *runtime, const char *part) {
    start_case(label);
    write_native_inputs(host_data);
    program run;
    CHECK(stage_native_host(language, runtime, &run) == 1);
    expect_contains(run.err, part, label);
    CHECK(!exists_in(base_folder, "bundle"));
    program_free(&run);
    finish_case();
}

static void refuses_host_data_naming_a_member_stage_bundle_writes_itself(void) {
    expect_native_host_refused("data-id", "{\"id\": \"other-host\"}", "fixture-one", "native", "names id");
    expect_native_host_refused("data-kind", "{\"kind\": \"interface\"}", "fixture-one", "native", "names kind");
    expect_native_host_refused("data-array", "[]", "fixture-one", "native", "is no JSON object");
}

static void refuses_a_language_that_is_no_language_name_and_a_runtime_outside_the_three(void) {
    expect_native_host_refused("spisor", "{}", "spisor", "native", "spisor is no language name");
    expect_native_host_refused("capital", "{}", "Fixture", "native", "Fixture is no language name");
    expect_native_host_refused("runtime", "{}", "fixture-one", "python", "python is no runtime");
}

static int stage_interface(const char *id, program *run) {
    char *directory = in_base("interface");
    char *binary = in_base("built/drawer.exe");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    const char *arguments[] = {"interface", directory, "0.4.0", id, binary, core, standard, NULL};
    int code = stage(arguments, run);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(binary);
    spisor_memory_release(directory);
    return code;
}

static void stages_an_interface_binary_under_its_platform(void) {
    start_case("interface");
    write_libraries();
    write_in(base_folder, "built/drawer.exe", "the interface program");
    program run;
    CHECK(stage_interface("fixture-interface", &run) == 0);
    check_text(run.err, "", "nothing refused");
    char *directory = in_base("interface");
    char key[256];
    char library[64];
    per_platform("drawer.exe", key, sizeof key);
    expect_file(directory, key, "the interface program");
    shared_library_name("weivel_core", library, sizeof library);
    per_platform(library, key, sizeof key);
    expect_file(directory, key, "the core");
    char expected[512];
    snprintf(expected, sizeof expected,
             "{\"kind\": \"interface\", \"id\": \"fixture-interface\", \"version\": \"0.4.0\", "
             "\"runtime\": \"native\", \"entry\": \"%s/drawer.exe\"}",
             runtime_platform_key());
    expect_manifest(directory, expected);
    spisor_memory_release(directory);
    program_free(&run);
    finish_case();
}

static void refuses_an_interface_id_no_catalog_can_list(void) {
    start_case("interface-id");
    write_libraries();
    write_in(base_folder, "built/drawer.exe", "the interface program");
    program run;
    CHECK(stage_interface("two words", &run) == 1);
    expect_contains(run.err, "two words is no item id", "names the id");
    CHECK(!exists_in(base_folder, "interface"));
    program_free(&run);
    finish_case();
}

static cJSON *built_catalog(const char *const *directories) {
    char *out = in_base("catalog");
    char hosts[3][2048];
    const char *arguments[12] = {"catalog", "build"};
    size_t count = 2;
    for (size_t index = 0; directories[index] != NULL; index++) {
        snprintf(hosts[index], sizeof hosts[index], "%s@%s", directories[index], runtime_platform_key());
        arguments[count++] = "--host";
        arguments[count++] = hosts[index];
    }
    arguments[count++] = "--out";
    arguments[count++] = out;
    arguments[count] = NULL;
    program run;
    CHECK(run_in(SPISOR_HOST_PATH, arguments, base_folder, &run) == 0);
    check_text(run.err, "", "the catalog builds");
    cJSON *catalog = json_in(out, "catalog.json");
    program_free(&run);
    spisor_memory_release(out);
    return catalog;
}

/** The files the catalog lists for item id at version on this platform. */
static const cJSON *platform_files(const cJSON *catalog, const char *id, const char *version) {
    char rest[512];
    snprintf(rest, sizeof rest, "items/%s/versions/%s/platforms/%s/files", id, version, runtime_platform_key());
    return value_at(catalog, rest);
}

static void lists_each_staged_kind_per_platform_and_spisor_host_as_a_host(void) {
    start_case("catalog");
    write_native_inputs("{}");
    write_in(base_folder, "built/drawer.exe", "the interface program");
    program run;
    CHECK(stage_native_host("fixture-one", "native", &run) == 0);
    program_free(&run);
    CHECK(stage_interface("fixture-interface", &run) == 0);
    program_free(&run);
    char *language_host = in_base("bundle");
    char *interface_item = in_base("interface");
    char *host = joined(STAGED_BUNDLES_PATH, "spisor-host");
    const char *directories[4] = {language_host, interface_item, host, NULL};
    cJSON *catalog = built_catalog(directories);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    const cJSON *hosting = cJSON_GetObjectItemCaseSensitive(items, "fixture-one-host");
    check_text(text_at(hosting, "kind"), "language-host", "the language host's kind");
    check_text(text_at(hosting, "language"), "fixture-one", "its language");
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(items, "fixture-interface"), "kind"), "interface",
               "the interface's kind");
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(items, "spisor-host"), "kind"), "host",
               "spisor-host still a host");
    char key[256];
    per_platform("weivel-host", key, sizeof key);
    CHECK(cJSON_HasObjectItem(platform_files(catalog, "fixture-one-host", "3.2.1"), key));
    per_platform("drawer.exe", key, sizeof key);
    CHECK(cJSON_HasObjectItem(platform_files(catalog, "fixture-interface", "0.4.0"), key));
    cJSON_Delete(catalog);
    spisor_memory_release(host);
    spisor_memory_release(interface_item);
    spisor_memory_release(language_host);
    finish_case();
}

/**
 * A weivel/typescript checkout as the typescript-host script reads one: its package.json naming the Node it needs, a
 * bundler whose program word writes the two program files and prints their paths, and koffi with this platform's
 * prebuilt and another's.
 */
static char *fake_checkout(const char *engine) {
    char *checkout = in_base("typescript");
    char package[256];
    snprintf(package, sizeof package, "{\"type\": \"module\", \"engines\": {\"node\": \"%s\"}}", engine);
    write_in(checkout, "package.json", package);
    write_in(checkout, "dist/bundle/bin.js",
             "import { writeFileSync } from \"node:fs\";\n"
             "import { join } from \"node:path\";\n"
             "const [word, directory] = process.argv.slice(2);\n"
             "if (word !== \"program\" || process.argv.length !== 4) { console.error(\"usage: weivel-bundle\"); "
             "process.exit(2); }\n"
             "for (const name of [\"weivel-host.mjs\", \"weivel-host-in-process.mjs\"]) {\n"
             "  writeFileSync(join(directory, name), name);\n"
             "  console.log(join(directory, name));\n"
             "}\n");
    write_in(checkout, "node_modules/koffi/package.json", "{\"name\": \"koffi\", \"version\": \"2.16.3\"}");
    write_in(checkout, "node_modules/koffi/index.js", "module.exports = {};\n");
    write_in(checkout, "node_modules/koffi/LICENSE.txt", "MIT");
    write_in(checkout, "node_modules/koffi/src/koffi.cc", "the sources");
    char triplet[64];
    char rest[256];
    koffi_triplet(triplet, sizeof triplet);
    snprintf(rest, sizeof rest, "node_modules/koffi/build/koffi/%s/koffi.node", triplet);
    write_in(checkout, rest, "this platform's koffi");
    write_in(checkout, "node_modules/koffi/build/koffi/" OTHER_TRIPLET "/koffi.node", "another platform's koffi");
    return checkout;
}

/** The builds of a pin: platform's one, its archive at url with sha256. */
static cJSON *builds_of(const char *platform, const char *url, const char *sha256) {
    cJSON *builds = cJSON_CreateObject();
    cJSON *build = cJSON_AddObjectToObject(builds, platform);
    cJSON_AddStringToObject(build, "url", url);
    cJSON_AddStringToObject(build, "sha256", sha256);
    return builds;
}

/** A pin of Node 26.0.1 holding builds, which it takes, as text (freed). */
static char *pin_text(cJSON *builds) {
    cJSON *pin = cJSON_CreateObject();
    cJSON_AddStringToObject(pin, "pinned", "26.0.1");
    cJSON_AddItemToObject(pin, "builds", builds);
    char *printed = cJSON_PrintUnformatted(pin);
    char *text = copy_text(printed);
    cJSON_free(printed);
    cJSON_Delete(pin);
    return text;
}

/** This platform's build, an https zip with a real-shaped SHA-256. */
static cJSON *good_builds(void) {
    return builds_of(runtime_platform_key(), "https://nodejs.org/dist/v26.0.1/node.zip", SHA256);
}

/** A -D<name>=<value> argument (freed). */
static char *definition(const char *name, const char *value) {
    size_t length = strlen(name) + strlen(value) + 4;
    char *text = spisor_memory_allocate(length);
    snprintf(text, length, "-D%s=%s", name, value);
    return text;
}

/**
 * Runs the typescript_host_bundle target's script into <base>/bundle at 9.8.7 over checkout, with the pin builds and
 * two libraries, as the target runs it; its exit code.
 */
static int stage_typescript_host_with(const char *checkout, cJSON *builds, program *run) {
    write_libraries();
    char *pin = in_base("node.json");
    char *text = pin_text(builds);
    write_file(pin, text);
    spisor_memory_release(text);
    char *directory = in_base("bundle");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    char *scratch = in_base("inputs");
    const char *names[8] = {"STAGE_BUNDLE", "DIRECTORY", "VERSION", "CHECKOUT", "NODE_PIN", "CORE", "STANDARD",
                            "SCRATCH"};
    const char *values[8] = {STAGE_BUNDLE_PATH, directory, "9.8.7", checkout, pin, core, standard, scratch};
    char *definitions[8];
    const char *arguments[11];
    for (size_t index = 0; index < 8; index++) {
        definitions[index] = definition(names[index], values[index]);
        arguments[index] = definitions[index];
    }
    arguments[8] = "-P";
    arguments[9] = TYPESCRIPT_HOST_SCRIPT;
    arguments[10] = NULL;
    int code = run_in(CMAKE_PATH, arguments, base_folder, run);
    for (size_t index = 0; index < 8; index++) {
        spisor_memory_release(definitions[index]);
    }
    spisor_memory_release(scratch);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(directory);
    spisor_memory_release(pin);
    return code;
}

static int stage_typescript_host(const char *checkout, program *run) {
    return stage_typescript_host_with(checkout, good_builds(), run);
}

static void expect_typescript_bundle_files(const char *directory) {
    char core[64];
    char standard[64];
    char triplet[64];
    char rest[256];
    shared_library_name("weivel_core", core, sizeof core);
    shared_library_name("weivel_standard", standard, sizeof standard);
    koffi_triplet(triplet, sizeof triplet);
    snprintf(rest, sizeof rest, "node_modules/koffi/build/koffi/%s/koffi.node", triplet);
    expect_file(directory, "weivel-host.mjs", "weivel-host.mjs");
    expect_file(directory, "weivel-host-in-process.mjs", "weivel-host-in-process.mjs");
    expect_file(directory, core, "the core");
    expect_file(directory, standard, "the standard");
    expect_file(directory, rest, "this platform's koffi");
    expect_file(directory, "node_modules/koffi/package.json", "{\"name\": \"koffi\", \"version\": \"2.16.3\"}");
    expect_file(directory, "node_modules/koffi/index.js", "module.exports = {};\n");
    expect_file(directory, "node_modules/koffi/LICENSE.txt", "MIT");
    CHECK(!exists_in(directory, "node_modules/koffi/build/koffi/" OTHER_TRIPLET));
    CHECK(!exists_in(directory, "node_modules/koffi/src"));
}

static void expect_typescript_host_manifest(const char *directory) {
    cJSON *manifest = json_in(directory, "host.json");
    cJSON *expected = parsed("{\"kind\": \"language-host\", \"id\": \"typescript-host\", \"version\": \"9.8.7\", "
                             "\"language\": \"typescript\", \"runtime\": \"javascript\", "
                             "\"entry\": \"weivel-host.mjs\", \"inProcessEntry\": \"weivel-host-in-process.mjs\", "
                             "\"node\": {\"pinned\": \"26.0.1\", \"minimum\": \"22.18.0\"}}");
    cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(expected, "node"), "builds", good_builds());
    if (manifest == NULL || !cJSON_Compare(manifest, expected, 1)) {
        char *printed = manifest == NULL ? NULL : cJSON_PrintUnformatted(manifest);
        fprintf(stderr, "host.json: %s\n", printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
    cJSON_Delete(manifest);
}

static void expect_catalog_of_one_typescript_host(const char *directory) {
    const char *directories[2] = {directory, NULL};
    cJSON *catalog = built_catalog(directories);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(items, "typescript-host");
    CHECK(cJSON_GetArraySize(items) == 1 && cJSON_GetObjectItemCaseSensitive(items, "koffi") == NULL);
    check_text(text_at(host, "kind"), "language-host", "kind");
    check_text(text_at(host, "language"), "typescript", "language");
    check_text(text_at(host, "runtime"), "javascript", "runtime");
    char triplet[64];
    char rest[512];
    koffi_triplet(triplet, sizeof triplet);
    const cJSON *files = platform_files(catalog, "typescript-host", "9.8.7");
    snprintf(rest, sizeof rest, "node_modules/koffi/build/koffi/%s/koffi.node", triplet);
    CHECK(cJSON_HasObjectItem(files, "host.json") && cJSON_HasObjectItem(files, "weivel-host.mjs") &&
          cJSON_HasObjectItem(files, rest));
    cJSON_Delete(catalog);
}

static void the_target_stages_the_program_bundle_beside_its_libraries_koffi_and_the_node_pin(void) {
    start_case("staged");
    char *checkout = fake_checkout(">=22.18.0");
    program run;
    CHECK(stage_typescript_host(checkout, &run) == 0);
    char *directory = in_base("bundle");
    expect_typescript_bundle_files(directory);
    expect_typescript_host_manifest(directory);
    expect_catalog_of_one_typescript_host(directory);
    spisor_memory_release(directory);
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

/** text with each run of whitespace made one space, undoing how CMake wraps a message; freed by the caller. */
static char *unwrapped(const char *text) {
    char *flat = copy_text(text == NULL ? "" : text);
    size_t length = 0;
    for (const char *at = flat; *at != 0; at++) {
        int space = *at == ' ' || *at == '\n' || *at == '\r' || *at == '\t';
        if (!space || length == 0 || flat[length - 1] != ' ') {
            flat[length++] = space ? ' ' : *at;
        }
    }
    flat[length] = 0;
    return flat;
}

/** The target's script refuses the checkout's engine or the pin builds, naming part, and stages nothing. */
static void expect_typescript_host_refused(const char *label, const char *engine, cJSON *builds, const char *part) {
    start_case(label);
    char *checkout = fake_checkout(engine);
    program run;
    CHECK(stage_typescript_host_with(checkout, builds, &run) != 0);
    char *message = unwrapped(run.err);
    expect_contains(message, part, label);
    spisor_memory_release(message);
    CHECK(!exists_in(base_folder, "bundle"));
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

static void the_target_refuses_a_pinned_node_older_than_the_one_the_program_needs(void) {
    expect_typescript_host_refused("too-old", ">=26.1.0", good_builds(), "pins Node 26.0.1, older than the 26.1.0");
}

static void the_target_refuses_an_engine_that_names_no_lower_bound(void) {
    expect_typescript_host_refused("engine", "^22.18.0", good_builds(), "^22.18.0");
}

static void the_target_refuses_a_build_that_is_not_an_https_archive_with_a_sha256(void) {
    cJSON *http = good_builds();
    cJSON *other = cJSON_AddObjectToObject(http, "linux-arm64");
    cJSON_AddStringToObject(other, "url", "http://nodejs.org/node.tar.gz");
    cJSON_AddStringToObject(other, "sha256", SHA256);
    expect_typescript_host_refused("http", ">=22.18.0", http, "http://nodejs.org/node.tar.gz");
    expect_typescript_host_refused("no-archive", ">=22.18.0",
                                   builds_of(runtime_platform_key(), "https://nodejs.org/node.exe", SHA256),
                                   "https://nodejs.org/node.exe");
    expect_typescript_host_refused("upper-sha", ">=22.18.0",
                                   builds_of(runtime_platform_key(), "https://nodejs.org/node.zip",
                                             "E2EDDA75BFB089654705CF65A13B58933BFD5F57148E8EBDA916F58A2474F1F7"),
                                   "E2EDDA75");
    expect_typescript_host_refused("short-sha", ">=22.18.0",
                                   builds_of(runtime_platform_key(), "https://nodejs.org/node.zip", "aa"),
                                   "not a sha256");
}

static void the_target_refuses_a_pin_with_no_build_for_this_platform(void) {
    expect_typescript_host_refused("no-platform", ">=22.18.0",
                                   builds_of("aix-ppc64", "https://nodejs.org/node.tar.gz", SHA256),
                                   runtime_platform_key());
}

static void answers_2_for_a_mode_missing_an_argument(void) {
    start_case("usage");
    const char *language_host[] = {"language-host", "bundle", "1.0.0", "fixture-one", NULL};
    const char *interface_item[] = {"interface", "bundle", "1.0.0", NULL};
    const char *retired[] = {"typescript-host", "bundle", "1.0.0", "checkout", "node.json", "core", "standard", NULL};
    program run;
    CHECK(stage(language_host, &run) == 2);
    expect_contains(run.err, "stage-bundle language-host", "the usage");
    program_free(&run);
    CHECK(stage(interface_item, &run) == 2);
    expect_contains(run.err, "stage-bundle interface", "the usage");
    program_free(&run);
    CHECK(stage(retired, &run) == 2);
    program_free(&run);
    finish_case();
}

static void prints_the_platform_a_bundle_is_staged_for(void) {
    start_case("platform");
    const char *arguments[] = {"platform", NULL};
    program run;
    CHECK(stage(arguments, &run) == 0);
    char expected[64];
    snprintf(expected, sizeof expected, "%s\n", runtime_platform_key());
    CHECK(run.out != NULL && (strcmp(run.out, expected) == 0 || strncmp(run.out, runtime_platform_key(),
                                                                         strlen(runtime_platform_key())) == 0));
    program_free(&run);
    finish_case();
}

static int node_is_on_the_path(void) {
    cJSON *environment = runtime_process_environment();
    char *node = NULL;
    runtime_on_path_status found = runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, "node", environment, &node);
    spisor_memory_release(node);
    cJSON_Delete(environment);
    return found == RUNTIME_ON_PATH_FOUND;
}

/** What a language-host call names beside its fixed words: NULL for the defaults of write_native_inputs. */
typedef struct language_host_call {
    const char *version;
    const char *entry;
    const char *data;
    const char *core;
    const char *file;
} language_host_call;

/** Stages fixture-one's native host into <base>/bundle from <base>/inputs as call names it; its exit code. */
static int stage_language_host_call(const language_host_call *call, program *run) {
    char *directory = in_base("bundle");
    char *data = in_base("host-data.json");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    char *inputs = in_base("inputs");
    const char *arguments[] = {"language-host", directory, call->version == NULL ? "3.2.1" : call->version,
                               "fixture-one", "native", call->entry == NULL ? "weivel-host" : call->entry,
                               call->data == NULL ? data : call->data, call->core == NULL ? core : call->core,
                               standard, call->file == NULL ? "data" : call->file, NULL};
    int code = run_in(STAGE_BUNDLE_PATH, arguments, inputs, run);
    spisor_memory_release(inputs);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(data);
    spisor_memory_release(directory);
    return code;
}

/** Whether stage-bundle left its staging folder behind beside directory_name in the base. */
static int staging_left(const char *directory_name) {
    char name[256];
    snprintf(name, sizeof name, "%s.staging", directory_name);
    return exists_in(base_folder, name);
}

/** The call is refused naming part, the bundle a prior call staged byte-identical and no staging left. */
static void expect_call_refused_keeping(const language_host_call *call, const char *part, const char *label) {
    char *bundle = in_base("bundle");
    size_t before_length = 0;
    size_t after_length = 0;
    char *before = tree_snapshot_sized(bundle, NULL, &before_length);
    program run;
    CHECK(stage_language_host_call(call, &run) == 1);
    expect_contains(run.err, part, label);
    char *after = tree_snapshot_sized(bundle, NULL, &after_length);
    if (before_length != after_length || memcmp(before, after, before_length) != 0) {
        fprintf(stderr, "%s: the bundle staged before changed\n", label);
        check_failures++;
    }
    CHECK(!staging_left("bundle"));
    program_free(&run);
    spisor_memory_release(after);
    spisor_memory_release(before);
    spisor_memory_release(bundle);
}

/** The call is refused naming part with no bundle and no staging folder written. */
static void expect_call_refused_writing_nothing(const language_host_call *call, const char *part, const char *label) {
    program run;
    CHECK(stage_language_host_call(call, &run) == 1);
    expect_contains(run.err, part, label);
    CHECK(!exists_in(base_folder, "bundle") && !staging_left("bundle"));
    program_free(&run);
}

static void a_refused_language_host_leaves_the_bundle_staged_before_untouched(void) {
    start_case("keeps");
    write_native_inputs("{}");
    program run;
    CHECK(stage_native_host("fixture-one", "native", &run) == 0);
    program_free(&run);
    language_host_call climbing = {NULL, NULL, NULL, NULL, "../x"};
    expect_call_refused_keeping(&climbing, "../x", "a file climbing out");
    char *missing_core = in_base("built/no-core.bin");
    language_host_call no_library = {NULL, NULL, NULL, missing_core, NULL};
    expect_call_refused_keeping(&no_library, "no-core.bin", "a missing library");
    language_host_call missing_entry = {NULL, "absent-host", NULL, NULL, NULL};
    expect_call_refused_keeping(&missing_entry, "absent-host", "a missing entry");
    spisor_memory_release(missing_core);
    finish_case();
}

static void a_refused_interface_leaves_the_bundle_staged_before_untouched(void) {
    start_case("keeps-interface");
    write_libraries();
    write_in(base_folder, "built/drawer.exe", "the interface program");
    program run;
    CHECK(stage_interface("fixture-interface", &run) == 0);
    program_free(&run);
    char *bundle = in_base("interface");
    size_t before_length = 0;
    size_t after_length = 0;
    char *before = tree_snapshot_sized(bundle, NULL, &before_length);
    char *directory = in_base("interface");
    char *missing = in_base("built/absent.exe");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    const char *arguments[] = {"interface", directory, "0.4.0", "fixture-interface", missing, core, standard, NULL};
    CHECK(stage(arguments, &run) == 1);
    expect_contains(run.err, "absent.exe", "the missing binary named");
    char *after = tree_snapshot_sized(bundle, NULL, &after_length);
    CHECK(before_length == after_length && memcmp(before, after, before_length) == 0);
    CHECK(!staging_left("interface"));
    program_free(&run);
    spisor_memory_release(after);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(missing);
    spisor_memory_release(directory);
    spisor_memory_release(before);
    spisor_memory_release(bundle);
    finish_case();
}

static void refuses_an_entry_or_file_that_is_no_relative_path_inside_the_working_directory(void) {
    start_case("paths");
    write_native_inputs("{}");
    char *absolute = in_base("inputs/weivel-host");
    const char *paths[7] = {"../x", absolute, "C:x", "a/.", "sub./a", "//server/share/x", "\\\\server\\share\\x"};
    for (size_t index = 0; index < 7; index++) {
        language_host_call as_entry = {NULL, paths[index], NULL, NULL, NULL};
        expect_call_refused_writing_nothing(&as_entry, "is not a relative path", paths[index]);
        language_host_call as_file = {NULL, NULL, NULL, NULL, paths[index]};
        expect_call_refused_writing_nothing(&as_file, "is not a relative path", paths[index]);
    }
    spisor_memory_release(absolute);
    finish_case();
}

static void refuses_a_link_at_the_top_or_inside_a_directory_writing_nothing(void) {
    start_case("links");
    write_native_inputs("{}");
    write_in(base_folder, "outside/secret.txt", "a secret");
    char *outside_file = in_base("outside/secret.txt");
    char *outside_folder = in_base("outside");
    char *file_link = in_base("inputs/linked-host");
    char *folder_link = in_base("inputs/linked-data");
    char *inner_link = in_base("inputs/data/linked");
    if (create_link_or_skip(file_link, outside_file) && create_folder_link_or_skip(folder_link, outside_folder) &&
        create_folder_link_or_skip(inner_link, outside_folder)) {
        language_host_call linked_entry = {NULL, "linked-host", NULL, NULL, NULL};
        expect_call_refused_writing_nothing(&linked_entry, "is a link, which a bundle cannot carry", "a linked entry");
        language_host_call linked_file = {NULL, NULL, NULL, NULL, "linked-data"};
        expect_call_refused_writing_nothing(&linked_file, "is a link, which a bundle cannot carry", "a linked folder");
        language_host_call inner = {NULL, NULL, NULL, NULL, NULL};
        expect_call_refused_writing_nothing(&inner, "is a link, which a bundle cannot carry", "a link inside data");
    }
    spisor_memory_release(inner_link);
    spisor_memory_release(folder_link);
    spisor_memory_release(file_link);
    spisor_memory_release(outside_folder);
    spisor_memory_release(outside_file);
    finish_case();
}

static void replaces_a_staging_folder_left_by_a_failed_run_and_refuses_a_linked_one(void) {
    start_case("staging");
    write_native_inputs("{}");
    write_in(base_folder, "bundle.staging/stale.txt", "left by a failed run");
    program run;
    CHECK(stage_native_host("fixture-one", "native", &run) == 0);
    CHECK(exists_in(base_folder, "bundle/host.json") && !exists_in(base_folder, "bundle/stale.txt"));
    CHECK(!staging_left("bundle"));
    program_free(&run);
    write_in(base_folder, "elsewhere/kept.txt", "kept");
    char *staging = in_base("bundle.staging");
    char *elsewhere = in_base("elsewhere");
    if (create_folder_link_or_skip(staging, elsewhere)) {
        CHECK(stage_native_host("fixture-one", "native", &run) == 1);
        expect_contains(run.err, "is a link", "the linked staging folder");
        CHECK(exists_in(base_folder, "elsewhere/kept.txt"));
        program_free(&run);
    }
    spisor_memory_release(elsewhere);
    spisor_memory_release(staging);
    finish_case();
}

static void refuses_a_version_no_catalog_can_list_in_either_mode(void) {
    start_case("version");
    write_native_inputs("{}");
    language_host_call call = {"three", NULL, NULL, NULL, NULL};
    expect_call_refused_writing_nothing(&call, "three is no version", "a language host's version");
    write_in(base_folder, "built/drawer.exe", "the interface program");
    char *directory = in_base("interface");
    char *binary = in_base("built/drawer.exe");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    const char *arguments[] = {"interface", directory, "1.0", "fixture-interface", binary, core, standard, NULL};
    program run;
    CHECK(stage(arguments, &run) == 1);
    expect_contains(run.err, "1.0 is no version", "an interface's version");
    CHECK(!exists_in(base_folder, "interface"));
    program_free(&run);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(binary);
    spisor_memory_release(directory);
    finish_case();
}

/** stage-bundle interface refuses the binary written as name, naming part, writing nothing. */
static void expect_binary_refused(const char *name, const char *part) {
    char rest[256];
    snprintf(rest, sizeof rest, "named/%s", name);
    write_in(base_folder, rest, "a binary");
    char *directory = in_base("interface");
    char *binary = in_base(rest);
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    const char *arguments[] = {"interface", directory, "0.4.0", "fixture-interface", binary, core, standard, NULL};
    program run;
    CHECK(stage(arguments, &run) == 1);
    expect_contains(run.err, part, name);
    CHECK(!exists_in(base_folder, "interface") && !staging_left("interface"));
    program_free(&run);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(binary);
    spisor_memory_release(directory);
}

static void refuses_an_interface_binary_named_as_a_library_or_no_catalog_name(void) {
    start_case("binary-name");
    write_libraries();
    char library[64];
    shared_library_name("weivel_core", library, sizeof library);
    expect_binary_refused(library, "the name of a library");
    shared_library_name("weivel_standard", library, sizeof library);
    expect_binary_refused(library, "the name of a library");
    expect_binary_refused("draw er", "is no file name a catalog can list");
    finish_case();
}

static void refuses_host_data_naming_the_application_or_a_member_twice(void) {
    expect_native_host_refused("data-application", "{\"application\": {}}", "fixture-one", "native",
                               "names application");
    expect_native_host_refused("data-twice", "{\"flavour\": 1, \"flavour\": 2}", "fixture-one", "native",
                               "names flavour twice");
}

static void says_a_missing_host_data_file_could_not_be_read(void) {
    start_case("no-data");
    write_native_inputs("{}");
    char *missing = in_base("absent-data.json");
    language_host_call call = {NULL, NULL, missing, NULL, NULL};
    expect_call_refused_writing_nothing(&call, "could not be read", "a missing host-data file");
    spisor_memory_release(missing);
    finish_case();
}

/** With the word node, the one case that runs the bundler on the PATH's node; else every case that needs none. */
int main(int count, char **arguments) {
    if (count == 2 && strcmp(arguments[1], "node") == 0) {
        if (!node_is_on_the_path()) {
            fputs("stage_bundle_test node: no node on the PATH, so the bundler cannot run\n", stderr);
            return 77;
        }
        the_target_stages_the_program_bundle_beside_its_libraries_koffi_and_the_node_pin();
        return check_summary("stage_bundle_test node");
    }
    answers_2_for_a_mode_missing_an_argument();
    prints_the_platform_a_bundle_is_staged_for();
    stages_a_native_language_host_under_its_platform_with_the_host_data_merged();
    refuses_host_data_naming_a_member_stage_bundle_writes_itself();
    refuses_host_data_naming_the_application_or_a_member_twice();
    says_a_missing_host_data_file_could_not_be_read();
    refuses_a_language_that_is_no_language_name_and_a_runtime_outside_the_three();
    refuses_a_version_no_catalog_can_list_in_either_mode();
    refuses_an_entry_or_file_that_is_no_relative_path_inside_the_working_directory();
    refuses_a_link_at_the_top_or_inside_a_directory_writing_nothing();
    a_refused_language_host_leaves_the_bundle_staged_before_untouched();
    replaces_a_staging_folder_left_by_a_failed_run_and_refuses_a_linked_one();
    stages_an_interface_binary_under_its_platform();
    refuses_an_interface_id_no_catalog_can_list();
    refuses_an_interface_binary_named_as_a_library_or_no_catalog_name();
    a_refused_interface_leaves_the_bundle_staged_before_untouched();
    lists_each_staged_kind_per_platform_and_spisor_host_as_a_host();
    the_target_refuses_a_pinned_node_older_than_the_one_the_program_needs();
    the_target_refuses_an_engine_that_names_no_lower_bound();
    the_target_refuses_a_build_that_is_not_an_https_archive_with_a_sha256();
    the_target_refuses_a_pin_with_no_build_for_this_platform();
    return summary_with_links("stage_bundle_test");
}
