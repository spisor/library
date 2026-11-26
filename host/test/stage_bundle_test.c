#include "host_fixture.h"
#include "spisor/allocation.h"

#include "on_path.h"
#include "process_environment.h"

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

/** The file name the bundled program looks for library under beside itself on this platform. */
static void shared_library_name(const char *library, char *name, size_t size) {
#if defined(_WIN32)
    snprintf(name, size, "%s.dll", library);
#elif defined(__APPLE__)
    snprintf(name, size, "lib%s.dylib", library);
#else
    snprintf(name, size, "lib%s.so", library);
#endif
}

/**
 * A weivel/typescript checkout as stage-bundle reads one: its package.json naming the Node it needs, a bundler whose
 * program word writes the two program files and prints their paths, and koffi with this platform's prebuilt and
 * another's.
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

static int stage(const char *const *arguments, program *run) {
    prepare_program(run, base_folder, "stage");
    cJSON *list = strings_of(arguments);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    copy_system_variable(environment, "PATH");
    program_start(run, STAGE_BUNDLE_PATH, list, environment, base_folder, 1);
    cJSON_Delete(environment);
    cJSON_Delete(list);
    return program_finish(run, 120000);
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

/** Runs stage-bundle javascript-host into <base>/bundle at 9.8.7 over checkout, with the pin builds and two libraries. */
static int stage_javascript_host_with(const char *checkout, cJSON *builds, program *run) {
    char *directory = in_base("bundle");
    char *pin = in_base("node.json");
    char *core = in_base("built/core.bin");
    char *standard = in_base("built/standard.bin");
    char *text = pin_text(builds);
    write_file(pin, text);
    spisor_memory_release(text);
    write_file(core, "the core");
    write_file(standard, "the standard");
    const char *arguments[] = {"javascript-host", directory, "9.8.7", checkout, pin, core, standard, NULL};
    int code = stage(arguments, run);
    spisor_memory_release(standard);
    spisor_memory_release(core);
    spisor_memory_release(pin);
    spisor_memory_release(directory);
    return code;
}

static int stage_javascript_host(const char *checkout, program *run) {
    return stage_javascript_host_with(checkout, good_builds(), run);
}

static void expect_file(const char *directory, const char *rest, const char *content) {
    char *text = text_in(directory, rest);
    check_text(text, content, rest);
    spisor_memory_release(text);
}

static void expect_bundle_files(const char *directory) {
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

static void expect_host_manifest(const char *directory) {
    cJSON *manifest = json_in(directory, "host.json");
    cJSON *expected = parsed("{\"id\": \"javascript-host\", \"version\": \"9.8.7\", \"runtime\": \"javascript\", "
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

static cJSON *built_catalog(const char *directory) {
    char *out = in_base("catalog");
    char host_argument[2048];
    snprintf(host_argument, sizeof host_argument, "%s@%s", directory, runtime_platform_key());
    const char *arguments[] = {"catalog", "build", "--host", host_argument, "--out", out, NULL};
    program run;
    prepare_program(&run, base_folder, "catalog");
    cJSON *list = strings_of(arguments);
    cJSON *environment = cJSON_CreateObject();
    copy_system_variable(environment, "SystemRoot");
    program_start(&run, SPISOR_HOST_PATH, list, environment, base_folder, 1);
    CHECK(program_finish(&run, 60000) == 0);
    check_text(run.err, "", "the catalog builds");
    cJSON *catalog = json_in(out, "catalog.json");
    program_free(&run);
    cJSON_Delete(environment);
    cJSON_Delete(list);
    spisor_memory_release(out);
    return catalog;
}

static void expect_catalog_of_one_javascript_host(const char *directory) {
    cJSON *catalog = built_catalog(directory);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(catalog, "items");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(items, "javascript-host");
    CHECK(cJSON_GetArraySize(items) == 1 && cJSON_GetObjectItemCaseSensitive(items, "koffi") == NULL);
    check_text(text_at(host, "kind"), "host", "kind");
    check_text(text_at(host, "runtime"), "javascript", "runtime");
    char triplet[64];
    char rest[512];
    koffi_triplet(triplet, sizeof triplet);
    snprintf(rest, sizeof rest, "versions/9.8.7/platforms/%s/files", runtime_platform_key());
    const cJSON *files = value_at(host, rest);
    snprintf(rest, sizeof rest, "node_modules/koffi/build/koffi/%s/koffi.node", triplet);
    CHECK(cJSON_HasObjectItem(files, "host.json") && cJSON_HasObjectItem(files, "weivel-host.mjs") &&
          cJSON_HasObjectItem(files, rest));
    cJSON_Delete(catalog);
}

static void stages_the_program_bundle_beside_its_libraries_koffi_and_the_node_pin(void) {
    start_case("staged");
    char *checkout = fake_checkout(">=22.18.0");
    program run;
    CHECK(stage_javascript_host(checkout, &run) == 0);
    check_text(run.err, "", "nothing refused");
    char *directory = in_base("bundle");
    expect_bundle_files(directory);
    expect_host_manifest(directory);
    expect_catalog_of_one_javascript_host(directory);
    spisor_memory_release(directory);
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

static void refuses_a_pinned_node_older_than_the_one_the_program_needs(void) {
    start_case("too-old");
    char *checkout = fake_checkout(">=26.1.0");
    program run;
    CHECK(stage_javascript_host(checkout, &run) == 1);
    expect_contains(run.err, "26.0.1", "names the pin");
    expect_contains(run.err, "26.1.0", "names the minimum");
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

static void refuses_an_engine_that_names_no_lower_bound(void) {
    start_case("engine");
    char *checkout = fake_checkout("^22.18.0");
    program run;
    CHECK(stage_javascript_host(checkout, &run) == 1);
    expect_contains(run.err, "^22.18.0", "names the engine");
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

/** stage-bundle refuses the pin builds, naming part, and stages nothing. */
static void expect_builds_refused(const char *label, cJSON *builds, const char *part) {
    start_case(label);
    char *checkout = fake_checkout(">=22.18.0");
    program run;
    CHECK(stage_javascript_host_with(checkout, builds, &run) == 1);
    expect_contains(run.err, part, label);
    CHECK(!exists_in(base_folder, "bundle"));
    program_free(&run);
    spisor_memory_release(checkout);
    finish_case();
}

static void refuses_a_build_that_is_not_an_https_archive_with_a_sha256(void) {
    cJSON *http = good_builds();
    cJSON *other = cJSON_AddObjectToObject(http, "linux-arm64");
    cJSON_AddStringToObject(other, "url", "http://nodejs.org/node.tar.gz");
    cJSON_AddStringToObject(other, "sha256", SHA256);
    expect_builds_refused("http", http, "http://nodejs.org/node.tar.gz");
    expect_builds_refused("no-archive", builds_of(runtime_platform_key(), "https://nodejs.org/node.exe", SHA256),
                          "https://nodejs.org/node.exe");
    expect_builds_refused("upper-sha",
                          builds_of(runtime_platform_key(), "https://nodejs.org/node.zip",
                                    "E2EDDA75BFB089654705CF65A13B58933BFD5F57148E8EBDA916F58A2474F1F7"),
                          "E2EDDA75");
    expect_builds_refused("short-sha", builds_of(runtime_platform_key(), "https://nodejs.org/node.zip", "aa"),
                          "not a sha256");
}

static void refuses_a_pin_with_no_build_for_this_platform(void) {
    expect_builds_refused("no-platform", builds_of("aix-ppc64", "https://nodejs.org/node.tar.gz", SHA256),
                          runtime_platform_key());
}

static void answers_2_for_a_javascript_host_word_missing_an_argument(void) {
    start_case("usage");
    const char *arguments[] = {"javascript-host", "bundle", "1.0.0", NULL};
    program run;
    CHECK(stage(arguments, &run) == 2);
    expect_contains(run.err, "stage-bundle javascript-host", "the usage");
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

int main(void) {
    answers_2_for_a_javascript_host_word_missing_an_argument();
    refuses_a_pinned_node_older_than_the_one_the_program_needs();
    refuses_an_engine_that_names_no_lower_bound();
    refuses_a_build_that_is_not_an_https_archive_with_a_sha256();
    refuses_a_pin_with_no_build_for_this_platform();
    if (!node_is_on_the_path()) {
        fputs("stage_bundle_test: no node on the PATH, so the bundler cannot run; its refusals ran\n", stderr);
        return check_failures == 0 ? 77 : check_summary("stage_bundle_test");
    }
    stages_the_program_bundle_beside_its_libraries_koffi_and_the_node_pin();
    return check_summary("stage_bundle_test");
}
