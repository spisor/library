#include "install_fixture.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "boot.h"
#include "boot_hint.h"
#include "hint_fixture.h"
#include "cache.h"
#include "catalog_time.h"
#include "child_process.h"
#include "host_references.h"
#include "hosts.h"
#include "install_hooks.h"
#include "install_support.h"
#include "process_run.h"
#include "registry.h"

#define PLUGINS "[\"announcer\", \"viewer\"]"

/** The catalog of the ecosystem tests: the foundation and two native plugins, plus a plugin built for another platform. */
static void list_ecosystem(install_base *base) {
    list_foundation(base);
    list_plugin(base, "announcer", "1.0.0", "native");
    list_plugin(base, "viewer", "1.0.0", "native");
    list_foreign_plugin(base, "rival", "1.0.0");
    write_catalog(base);
}

static cJSON *install_defaults(const install_base *base, const char *extra_json) {
    char argument[512];
    snprintf(argument, sizeof argument, "{\"defaults\": " PLUGINS "%s%s}", extra_json[0] == 0 ? "" : ", ", extra_json);
    return expect_report(install_with(base, argument, NULL), "install");
}

static void expect_member(const cJSON *tree, const char *slashed, const char *expected_json, const char *label) {
    expect_json(value_at(tree, slashed), expected_json, label);
}

static void installs_the_host_the_runtime_and_the_defaults_and_records_them(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "first");
    list_ecosystem(&base);
    cJSON *report = install_defaults(&base, "");
    expect_installed_ids(report, "[\"announcer\",\"runtime\",\"spisor-host\",\"viewer\"]", "installed");
    expect_member(report, "cached", "[]", "cached");
    expect_member(report, "unavailable", "[]", "unavailable");
    expect_member(report, "enabled", "[\"announcer\",\"viewer\"]", "enabled");
    CHECK(cJSON_GetObjectItemCaseSensitive(report, "javascript") == NULL);
    char library[256];
    snprintf(library, sizeof library, "libraries/runtime/0.1.0/%s/library.bin", runtime_platform_key());
    CHECK(exists_in(base.cache, library) && exists_in(base.cache, "bundles/spisor-host/1.0.0/host.json"));
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"announcer\":{\"enabled\":true},\"viewer\":{\"enabled\":true}}", "plugins");
    char *index = index_path_of(&base);
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "kind", "local");
    cJSON_AddStringToObject(source, "path", index);
    char *source_json = cJSON_PrintUnformatted(source);
    char expected[2048];
    snprintf(expected, sizeof expected, "{\"sources\":[%s]}", source_json);
    expect_member(settings, "catalog", expected, "catalog sources");
    cJSON *references = json_in(base.state, "bundles/spisor-host.json");
    expect_member(references, "run",
                  "{\"spisor-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\",\"viewer\":\"1.0.0\"}", "run");
    cJSON *hint = json_in(base.cache, "hosts/spisor-host.json");
    cJSON *versions = hinted_versions(hint);
    expect_json(versions, "{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"1.0.0\"}", "boot hint");
    cJSON_Delete(versions);
    cJSON_Delete(hint);
    cJSON_Delete(references);
    cJSON_free(source_json);
    cJSON_Delete(source);
    free(index);
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void reports_everything_cached_on_a_second_run(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "second");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *report = install_defaults(&base, "");
    expect_member(report, "installed", "[]", "nothing fetched");
    expect_member(report, "enabled", "[]", "nothing enabled");
    expect_member(report, "cached", "[\"spisor-host@1.0.0\",\"runtime@0.1.0\",\"announcer@1.0.0\",\"viewer@1.0.0\"]",
                  "all cached");
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void leaves_a_plugin_the_user_disabled_disabled(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "disabled");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    set_portable(&base, "plugins.announcer.enabled", cJSON_CreateFalse());
    cJSON *report = install_defaults(&base, "");
    CHECK(!runtime_install_holds(cJSON_GetObjectItemCaseSensitive(report, "cached"), "announcer@1.0.0"));
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"announcer\":{\"enabled\":false},\"viewer\":{\"enabled\":true}}", "still disabled");
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void reports_an_item_with_no_build_for_this_platform_and_installs_the_rest(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "foreign");
    list_ecosystem(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"announcer\", \"viewer\", \"rival\"]}", NULL), "foreign");
    expect_installed_ids(report, "[\"announcer\",\"runtime\",\"spisor-host\",\"viewer\"]", "rest installed");
    const cJSON *unavailable = cJSON_GetObjectItemCaseSensitive(report, "unavailable");
    CHECK(cJSON_GetArraySize(unavailable) == 1);
    char expected[128];
    snprintf(expected, sizeof expected, "not built for %s", runtime_platform_key());
    const cJSON *row = cJSON_GetArrayItem(unavailable, 0);
    check_text(text_at(row, "id"), "rival", "unavailable id");
    check_text(text_at(row, "message"), expected, "unavailable message");
    CHECK(text_at(row, "fix") != NULL && text_at(row, "fix")[0] != 0);
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"announcer\":{\"enabled\":true},\"viewer\":{\"enabled\":true}}", "rival not enabled");
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void honours_a_pin_on_the_runtime_and_records_it_for_the_next_start(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "pin");
    list_library(&base, "runtime", "0.0.9");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    set_portable(&base, "plugins.runtime.pin", cJSON_CreateString("0.0.9"));
    cJSON *report = install_defaults(&base, "");
    expect_member(report, "installed", "[{\"id\":\"runtime\",\"version\":\"0.0.9\"}]", "pinned runtime");
    cJSON *hint = json_in(base.cache, "hosts/spisor-host.json");
    cJSON *versions = hinted_versions(hint);
    expect_member(versions, "libraries", "{\"runtime\":\"0.0.9\"}", "hinted runtime");
    cJSON_Delete(versions);
    cJSON_Delete(hint);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_and_enables_a_plugin_asked_for_by_name(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "requested");
    list_ecosystem(&base);
    cJSON_Delete(expect_report(install_with(&base, "{\"defaults\": [\"viewer\"]}", NULL), "viewer"));
    cJSON *report = expect_report(install_with(&base, "{\"requested\": [\"announcer\"]}", NULL), "requested");
    expect_member(report, "installed", "[{\"id\":\"announcer\",\"version\":\"1.0.0\"}]", "requested installed");
    expect_member(report, "enabled", "[\"announcer\"]", "requested enabled");
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"viewer\":{\"enabled\":true},\"announcer\":{\"enabled\":true}}", "both enabled");
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void enables_a_plugin_asked_for_by_name_even_when_the_user_turned_it_off(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "requested-off");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    set_portable(&base, "plugins.announcer.enabled", cJSON_CreateFalse());
    cJSON *report = expect_report(install_with(&base, "{\"requested\": [\"announcer\"]}", NULL), "requested off");
    expect_member(report, "enabled", "[\"announcer\"]", "enabled again");
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"announcer\":{\"enabled\":true},\"viewer\":{\"enabled\":true}}", "turned on");
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void keeps_the_versions_the_host_scheduled_for_its_next_start(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "next");
    list_ecosystem(&base);
    write_in(base.state, "bundles/spisor-host.json", "{\"run\": {}, \"next\": {\"viewer\": \"2.0.0\"}}");
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *references = json_in(base.state, "bundles/spisor-host.json");
    expect_member(references, "next", "{\"viewer\":\"2.0.0\"}", "next kept");
    cJSON_Delete(references);
    finish_install_base(&base);
}

static void records_as_run_only_what_this_install_installed(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "run-replaced");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    set_portable(&base, "plugins.viewer.enabled", cJSON_CreateFalse());
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *references = json_in(base.state, "bundles/spisor-host.json");
    expect_member(references, "run", "{\"spisor-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\"}",
                  "a disabled plugin leaves run");
    cJSON_Delete(references);
    finish_install_base(&base);
}

static void records_a_local_source_in_the_application_layer_never_the_portable_one(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "local-layer");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *application = ecosystem_settings(&base, 1, 0);
    cJSON *portable = ecosystem_settings(&base, 0, 1);
    CHECK(cJSON_GetArraySize(value_at(application, "catalog/sources")) == 1);
    CHECK(value_at(portable, "catalog") == NULL);
    cJSON_Delete(portable);
    cJSON_Delete(application);
    finish_install_base(&base);
}

static void appends_a_source_not_yet_listed_and_never_replaces_the_ones_that_are(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "append");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    write_catalog_of(&base, "second.json");
    char *second = joined(base.catalog, "second.json");
    char argument[2048];
    cJSON *quoted = cJSON_CreateString(second);
    char *second_json = cJSON_PrintUnformatted(quoted);
    snprintf(argument, sizeof argument, "{\"sources\": [{\"kind\": \"local\", \"path\": %s}]}", second_json);
    cJSON_Delete(expect_report(install_with(&base, argument, NULL), "second source"));
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    const cJSON *sources = value_at(settings, "catalog/sources");
    char *first = index_path_of(&base);
    CHECK(cJSON_GetArraySize(sources) == 2);
    check_text(text_at(cJSON_GetArrayItem(sources, 0), "path"), first, "first kept first");
    check_text(text_at(cJSON_GetArrayItem(sources, 1), "path"), second, "second appended");
    free(first);
    cJSON_Delete(settings);
    cJSON_free(second_json);
    cJSON_Delete(quoted);
    free(second);
    finish_install_base(&base);
}

static void registers_the_host_at_install_keeping_its_provenance_on_a_later_install(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "registers");
    list_ecosystem(&base);
    double before = runtime_now_milliseconds();
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *registration = json_in(base.state, "hosts/spisor-host.json");
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(registration, "provenance");
    check_text(text_at(provenance, "method"), "catalog", "provenance method");
    check_text(text_at(provenance, "version"), "1.0.0", "provenance version");
    char *location = joined(base.cache, "bundles/spisor-host/1.0.0");
    check_text(text_at(provenance, "location"), location, "provenance location");
    double installed_at = 0;
    CHECK(runtime_catalog_parse_time(text_at(provenance, "installedAt"), &installed_at));
    CHECK(installed_at >= before - 1000 && installed_at <= runtime_now_milliseconds());
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *again = json_in(base.state, "hosts/spisor-host.json");
    char *first = cJSON_PrintUnformatted(provenance);
    expect_json(cJSON_GetObjectItemCaseSensitive(again, "provenance"), first, "provenance kept");
    cJSON_free(first);
    cJSON_Delete(again);
    free(location);
    cJSON_Delete(registration);
    finish_install_base(&base);
}

#if defined(_WIN32)
#define EXECUTABLE_SUFFIX ".exe"
#else
#define EXECUTABLE_SUFFIX ""
#endif

#define OLD_HOST_FILE "bundles/forebay-host/1.0.0/host.json"
#define OLD_STUB_FILE "boot/forebay-boot" EXECUTABLE_SUFFIX
#define OLD_FILE_CONTENT "left by an earlier install"

/** The ecosystem catalog, its host build carrying the host executable and the boot stub for this platform. */
static void list_ecosystem_carrying_the_boot_stub(install_base *base) {
    list_foundation(base);
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(base->items, "spisor-host"), "versions");
    cJSON *files = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(versions, "1.0.0"), "files");
    char executable[128];
    snprintf(executable, sizeof executable, "%s/spisor-host" EXECUTABLE_SUFFIX, runtime_platform_key());
    cJSON_AddItemToObject(files, executable, served_reference(base, "spisor-host", "1.0.0", executable, "host"));
    char stub[128];
    snprintf(stub, sizeof stub, "%s/spisor-boot" EXECUTABLE_SUFFIX, runtime_platform_key());
    cJSON_AddItemToObject(files, stub, served_reference(base, "spisor-host", "1.0.0", stub, "stub"));
    list_plugin(base, "announcer", "1.0.0", "native");
    list_plugin(base, "viewer", "1.0.0", "native");
    write_catalog(base);
}

static void installs_beside_a_cache_laid_out_under_the_old_names(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "old-names");
    list_ecosystem_carrying_the_boot_stub(&base);
    write_in(base.cache, OLD_HOST_FILE, OLD_FILE_CONTENT);
    write_in(base.cache, OLD_STUB_FILE, OLD_FILE_CONTENT);
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", base.cache);
    catalog_answer placed = call_catalog(runtime_boot_place, argument);
    CHECK(placed.status == SPISOR_STATUS_SUCCESS && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(placed.tree, "placed")));
    char *stub = text_in(base.cache, "boot/spisor-boot" EXECUTABLE_SUFFIX);
    check_text(stub, "stub", "spisor-boot placed");
    cJSON *registration = json_in(base.state, "hosts/spisor-host.json");
    CHECK(registration != NULL);
    char *old_host = text_in(base.cache, OLD_HOST_FILE);
    char *old_stub = text_in(base.cache, OLD_STUB_FILE);
    check_text(old_host, OLD_FILE_CONTENT, "old host bundle untouched");
    check_text(old_stub, OLD_FILE_CONTENT, "old boot stub untouched");
    free(old_stub);
    free(old_host);
    cJSON_Delete(registration);
    free(stub);
    cJSON_Delete(placed.tree);
    finish_install_base(&base);
}

/** An https source nothing answers: its check fails at once, and it is recorded all the same. */
#define UNREACHABLE_SOURCE "https://127.0.0.1:1/catalog.json"

static void records_an_https_source_in_the_portable_layer_even_when_its_check_fails(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "https-source");
    list_ecosystem(&base);
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *sources = local_sources(&base);
    cJSON *https = cJSON_CreateObject();
    cJSON_AddStringToObject(https, "kind", "https");
    cJSON_AddStringToObject(https, "url", UNREACHABLE_SOURCE);
    cJSON_AddItemToArray(sources, https);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "sources", sources);
    char *extra = cJSON_PrintUnformatted(argument);
    cJSON_Delete(argument);
    cJSON *report = expect_report(install_with(&base, extra, NULL), "https source");
    cJSON_free(extra);
    const cJSON *failed = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(failed, "id"), "catalog", "failed check reported");
    cJSON *portable = ecosystem_settings(&base, 0, 1);
    expect_member(portable, "catalog", "{\"sources\":[{\"kind\":\"https\",\"url\":\"" UNREACHABLE_SOURCE "\"}]}",
                  "https in the portable layer");
    cJSON *merged = ecosystem_settings(&base, 1, 1);
    const cJSON *listed = value_at(merged, "catalog/sources");
    CHECK(cJSON_GetArraySize(listed) == 2);
    check_text(text_at(cJSON_GetArrayItem(listed, 1), "url"), UNREACHABLE_SOURCE, "https after the local source");
    cJSON_Delete(merged);
    cJSON_Delete(portable);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void refuses_a_source_that_is_neither_local_nor_https(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "bad-source");
    list_ecosystem(&base);
    catalog_answer answer = install_with(&base, "{\"sources\": [{\"kind\": \"ftp\", \"url\": \"ftp://x\"}]}", NULL);
    expect_refusal(answer, "no catalog source", "bad source kind");
    cJSON_Delete(answer.tree);
    answer = install_with(&base, "{\"sources\": [{\"kind\": \"local\"}]}", NULL);
    expect_refusal(answer, "no catalog source", "local source without a path");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "bundles"));
    finish_install_base(&base);
}

static void acquires_no_javascript_runtime_for_a_catalog_with_no_javascript_item(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "no-javascript");
    list_ecosystem(&base);
    list_library(&base, "koffi", "2.16.3");
    list_host(&base, "javascript-host", "1.0.0", "javascript", NULL);
    write_catalog(&base);
    runtime_install_use_runtime_build("1.2.3", "file:///nowhere/node.zip", "0000000000000000000000000000000000000000000000000000000000000000");
    cJSON *report = install_defaults(&base, "");
    CHECK(cJSON_GetObjectItemCaseSensitive(report, "javascript") == NULL);
    expect_member(report, "unavailable", "[]", "nothing unavailable");
    CHECK(!exists_in(base.cache, "runtimes") && !exists_in(base.cache, "libraries/koffi") &&
          !exists_in(base.cache, "bundles/javascript-host"));
    runtime_install_use_runtime_build(NULL, NULL, NULL);
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** A served runtime build install.runtime is pointed at: its archive's file: url and SHA-256. */
typedef struct served_build {
    char url[2048];
    char sha256[RUNTIME_SHA256_HEX_SIZE];
} served_build;

/** Bytes that do not compress, so an archive of them takes long enough to fetch and unpack that two installs overlap. */
static void write_incompressible(const char *path, size_t size) {
    char *bytes = malloc(size);
    unsigned long state = 2463534242UL;
    for (size_t index = 0; index < size; index++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = (char)(state & 0xff);
    }
    char *parent = joined(path, "..");
    CHECK(spisor_make_directories(parent) == 1);
    CHECK(spisor_replace_file(path, (const uint8_t *)bytes, size) == 1);
    free(parent);
    free(bytes);
}

/** Packs a one-file runtime archive of size bytes with the OS's tar and makes install.runtime acquire it. */
static void serve_runtime_build(install_base *base, size_t size, served_build *served) {
#if defined(_WIN32)
    const char *member = "node-v1.2.3-fixture/node.exe";
    const char *archive_name = "node-v1.2.3-fixture.zip";
#else
    const char *member = "node-v1.2.3-fixture/bin/node";
    const char *archive_name = "node-v1.2.3-fixture.tar.gz";
#endif
    char *packing = joined(base->root, "packing");
    char *executable = joined(packing, member);
    write_incompressible(executable, size);
    char *archive = joined(base->root, archive_name);
    cJSON *arguments = cJSON_CreateArray();
#if defined(_WIN32)
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    char narrow[4 * MAX_PATH];
    int written = WideCharToMultiByte(CP_UTF8, 0, folder, (int)length, narrow, (int)sizeof narrow - 1, NULL, NULL);
    narrow[written] = 0;
    char *tar = joined(narrow, "tar.exe");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-a"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-cf"));
#else
    char *tar = joined(spisor_file_exists("/usr/bin/tar") ? "/usr/bin" : "/bin", "tar");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-czf"));
#endif
    cJSON_AddItemToArray(arguments, cJSON_CreateString(archive));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-C"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(packing));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("node-v1.2.3-fixture"));
    int exit_code = -1;
    char *problem = NULL;
    CHECK(runtime_run_to_exit(tar, arguments, 120000, &exit_code, NULL, &problem) && exit_code == 0);
    CHECK(spisor_sha256_file(archive, served->sha256));
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, archive);
    snprintf(served->url, sizeof served->url, "%s", url);
    runtime_install_use_runtime_build("1.2.3", served->url, served->sha256);
    free(url);
    free(problem);
    free(tar);
    cJSON_Delete(arguments);
    free(archive);
    free(executable);
    free(packing);
}

static void acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "javascript");
    list_foundation(&base);
    list_plugin(&base, "scripted", "1.0.0", NULL);
    list_library(&base, "koffi", "2.16.3");
    list_library(&base, "forebay-library", "0.1.2");
    list_host(&base, "javascript-host", "1.0.0", "javascript", NULL);
    write_catalog(&base);
    served_build served;
    serve_runtime_build(&base, 64, &served);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"scripted\"]}", NULL), "javascript");
    expect_installed_ids(report, "[\"forebay-library\",\"javascript-host\",\"koffi\",\"runtime\",\"scripted\",\"spisor-host\"]", "with its host");
    const cJSON *javascript = cJSON_GetObjectItemCaseSensitive(report, "javascript");
    CHECK(javascript != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(javascript, "fetched")) &&
          spisor_file_exists(text_at(javascript, "executable")));
    cJSON *hint = json_in(base.cache, "hosts/javascript-host.json");
    cJSON *versions = hinted_versions(hint);
    expect_json(versions, "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"2.16.3\",\"forebay-library\":\"0.1.2\"},\"host\":\"1.0.0\"}", "javascript-host hint");
    cJSON_Delete(versions);
    cJSON_Delete(hint);
    runtime_install_use_runtime_build(NULL, NULL, NULL);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void refuses_an_argument_with_no_usable_places(void) {
    catalog_answer answer = call_catalog(runtime_install_ecosystem, parsed("{\"sources\": [], \"places\": {}, \"environment\": {}}"));
    expect_refusal(answer, "places", "no places");
    cJSON_Delete(answer.tree);
}

/** The base another process of this suite created, its catalog already written, entered by its root folder. */
static void enter_install_base(install_base *base, const char *root) {
    snprintf(base->root, sizeof base->root, "%s", root);
    base->portable = joined(base->root, "portable");
    base->state = joined(base->root, "state");
    base->cache = joined(base->root, "cache");
    base->user = joined(base->root, "user");
    base->path = joined(base->root, "path");
    base->catalog = joined(base->root, "catalog");
    base->items = NULL;
}

/** The child half of the concurrent test: one install requesting the comma-separated plugins, from the served build. */
static int run_child_install(const char *root, const char *requested, const char *url, const char *sha256) {
    install_base base;
    enter_install_base(&base, root);
    runtime_install_use_runtime_build("1.2.3", url, sha256);
    cJSON *argument = parsed("{\"choices\": {}}");
    cJSON *ids = cJSON_AddArrayToObject(argument, "requested");
    char copy[256];
    snprintf(copy, sizeof copy, "%s", requested);
    for (char *id = strtok(copy, ","); id != NULL; id = strtok(NULL, ",")) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(id));
    }
    char *extra = cJSON_PrintUnformatted(argument);
    cJSON_Delete(argument);
    catalog_answer answer = install_with(&base, extra, NULL);
    cJSON_free(extra);
    int succeeded = answer.status == SPISOR_STATUS_SUCCESS &&
                    cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "unavailable")) == 0;
    if (!succeeded) {
        char *printed = answer.tree == NULL ? NULL : cJSON_PrintUnformatted(answer.tree);
        fprintf(stderr, "child install: %s\n", printed == NULL ? "(nothing)" : printed);
        cJSON_free(printed);
    }
    cJSON_Delete(answer.tree);
    return succeeded ? 0 : 1;
}

static int child_exit_code(child_process *child) {
#if defined(_WIN32)
    DWORD code = 1;
    WaitForSingleObject(child->handle, 120000);
    GetExitCodeProcess(child->handle, &code);
    release_child(child);
    return (int)code;
#else
    int status = 0;
    waitpid((pid_t)child->pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

static void expect_intact(const install_base *base, const char *rest) {
    cJSON *argument = cJSON_CreateObject();
    char *directory = joined(base->cache, rest);
    cJSON_AddStringToObject(argument, "directory", directory);
    free(directory);
    catalog_answer answer = call_catalog(runtime_cache_check, argument);
    check_text(text_at(answer.tree, "kind"), "intact", rest);
    cJSON_Delete(answer.tree);
}

/** Five plugins for each of two installs, so their settings writes interleave many times. */
static void list_many_plugins(install_base *base) {
    for (int index = 1; index <= 5; index++) {
        char first[32];
        char second[32];
        snprintf(first, sizeof first, "first-%d", index);
        snprintf(second, sizeof second, "second-%d", index);
        list_plugin(base, first, "1.0.0", "native");
        list_plugin(base, second, "1.0.0", "native");
    }
}

/** Whether the JavaScript runtime the installs acquired is unpacked intact: its marker names the executable's bytes. */
static int runtime_is_intact(const install_base *base) {
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s", runtime_platform_key());
    char *directory = joined(base->cache, rest);
#if defined(_WIN32)
    char *executable = joined(directory, "node-v1.2.3-fixture/node.exe");
#else
    char *executable = joined(directory, "node-v1.2.3-fixture/bin/node");
#endif
    char *marker = text_in(directory, ".unpacked");
    char hash[RUNTIME_SHA256_HEX_SIZE];
    int intact = marker != NULL && strlen(marker) == 130 && spisor_sha256_file(executable, hash) &&
                 strncmp(marker + 65, hash, 64) == 0;
    free(marker);
    free(executable);
    free(directory);
    return intact;
}

/** No acquisition left a staging folder behind in the runtime's version folder. */
static void expect_no_staging_left(const install_base *base) {
    char *versions = joined(base->cache, "runtimes/node/1.2.3");
    char prefix[64];
    snprintf(prefix, sizeof prefix, ".%s-staging-", runtime_platform_key());
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(versions, &listing) == 1);
    for (size_t index = 0; index < listing.count; index++) {
        if (strncmp(listing.entries[index].name, prefix, strlen(prefix)) == 0) {
            fprintf(stderr, "a staging folder was left: %s\n", listing.entries[index].name);
            check_failures++;
        }
    }
    runtime_free_directory_listing(&listing);
    free(versions);
}

/** What runtimes/javascript.json records: the acquired pin at its final executable. */
static void expect_recorded_runtime(const install_base *base) {
    cJSON *record = json_in(base->cache, "runtimes/javascript.json");
    char rest[256];
#if defined(_WIN32)
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s/node-v1.2.3-fixture/node.exe", runtime_platform_key());
#else
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s/node-v1.2.3-fixture/bin/node", runtime_platform_key());
#endif
    char *executable = joined(base->cache, rest);
    check_text(text_at(record, "name"), "node", "recorded name");
    check_text(text_at(record, "version"), "1.2.3", "recorded version");
    check_text(text_at(record, "source"), "acquired", "recorded source");
    check_text(text_at(record, "executable"), executable, "recorded executable");
    CHECK(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(record, "fetched")));
    free(executable);
    cJSON_Delete(record);
}

/**
 * Two installs at once over one base, asking for different plugins and both acquiring the JavaScript
 * runtime, end with one consistent registry and cache, neither failing and neither losing the other's
 * settings.
 */
static void two_concurrent_installs_end_with_one_consistent_registry_and_cache(const char *self) {
    install_base base;
    start_install_base(&base, "ecosystem", "concurrent");
    list_ecosystem(&base);
    list_plugin(&base, "scripted", "1.0.0", NULL);
    list_many_plugins(&base);
    list_library(&base, "koffi", "2.16.3");
    list_library(&base, "forebay-library", "0.1.2");
    list_host(&base, "javascript-host", "1.0.0", "javascript", NULL);
    cJSON *application = application_json("claude", "claude", "{}");
    list_loader(&base, "claude-loader", "1.0.0", application, "[]", 0, "[\"keeper\"]");
    list_host(&base, "keeper", "1.0.0", "native", NULL);
    write_catalog(&base);
    served_build served;
    serve_runtime_build(&base, 24 * 1024 * 1024, &served);
    write_in(base.path, "claude.cmd", "@echo off\r\n");
#if !defined(_WIN32)
    char *binary = joined(base.path, "claude");
    write_file(binary, "#!/bin/sh\n");
    CHECK(chmod(binary, 0755) == 0);
    free(binary);
#endif
    const char *first_arguments[5] = {"--child", base.root, "announcer,first-1,first-2,first-3,first-4,first-5,scripted", served.url, served.sha256};
    const char *second_arguments[5] = {"--child", base.root, "viewer,second-1,second-2,second-3,second-4,second-5,scripted", served.url, served.sha256};
    child_process first = start_child(self, first_arguments, 5, 0, NULL);
    child_process second = start_child(self, second_arguments, 5, 0, NULL);
    CHECK(first.pid != 0 && second.pid != 0);
    CHECK(child_exit_code(&first) == 0);
    CHECK(child_exit_code(&second) == 0);
    cJSON *listed = cJSON_CreateObject();
    cJSON_AddItemToObject(listed, "places", places_of(&base));
    catalog_answer hosts = call_catalog(runtime_hosts_list, listed);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hosts.tree, "hosts")) == 3);
    expect_json(cJSON_GetObjectItemCaseSensitive(hosts.tree, "unreadable"), "[]", "no unreadable registration");
    cJSON_Delete(hosts.tree);
    const char *versions[8] = {"bundles/spisor-host/1.0.0", "bundles/announcer/1.0.0", "bundles/viewer/1.0.0",
                               "bundles/scripted/1.0.0", "bundles/javascript-host/1.0.0", "bundles/claude-loader/1.0.0",
                               "bundles/keeper/1.0.0", NULL};
    for (size_t index = 0; versions[index] != NULL; index++) {
        expect_intact(&base, versions[index]);
    }
    CHECK(runtime_is_intact(&base));
    expect_no_staging_left(&base);
    expect_recorded_runtime(&base);
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    const char *enabled[13] = {"announcer", "viewer", "scripted", "first-1", "first-2", "first-3", "first-4",
                               "first-5", "second-1", "second-2", "second-3", "second-4", "second-5"};
    for (size_t index = 0; index < 13; index++) {
        char slashed[64];
        snprintf(slashed, sizeof slashed, "plugins/%s/enabled", enabled[index]);
        if (!cJSON_IsTrue(value_at(settings, slashed))) {
            fprintf(stderr, "both installs' plugins enabled: %s is not\n", enabled[index]);
            check_failures++;
        }
    }
    CHECK(cJSON_GetArraySize(value_at(settings, "catalog/sources")) == 1);
    cJSON *references = json_in(base.state, "bundles/spisor-host.json");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(references, "run");
    CHECK(cJSON_HasObjectItem(run, "spisor-host") && cJSON_HasObjectItem(run, "scripted") &&
          (cJSON_HasObjectItem(run, "announcer") || cJSON_HasObjectItem(run, "viewer")));
    cJSON_Delete(settings);
    cJSON_Delete(references);
    finish_install_base(&base);
}

static const char *self_path(const char *argument) {
#if defined(_WIN32)
    static char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? argument : path;
#else
    return argument;
#endif
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "--child") == 0) {
        return run_child_install(argv[2], argv[3], argv[4], argv[5]);
    }
    user_path_guard guard;
    guard_user_path(&guard);
    installs_the_host_the_runtime_and_the_defaults_and_records_them();
    reports_everything_cached_on_a_second_run();
    leaves_a_plugin_the_user_disabled_disabled();
    reports_an_item_with_no_build_for_this_platform_and_installs_the_rest();
    honours_a_pin_on_the_runtime_and_records_it_for_the_next_start();
    installs_and_enables_a_plugin_asked_for_by_name();
    enables_a_plugin_asked_for_by_name_even_when_the_user_turned_it_off();
    keeps_the_versions_the_host_scheduled_for_its_next_start();
    records_as_run_only_what_this_install_installed();
    records_a_local_source_in_the_application_layer_never_the_portable_one();
    appends_a_source_not_yet_listed_and_never_replaces_the_ones_that_are();
    registers_the_host_at_install_keeping_its_provenance_on_a_later_install();
    installs_beside_a_cache_laid_out_under_the_old_names();
    records_an_https_source_in_the_portable_layer_even_when_its_check_fails();
    refuses_a_source_that_is_neither_local_nor_https();
    acquires_no_javascript_runtime_for_a_catalog_with_no_javascript_item();
    acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin();
    refuses_an_argument_with_no_usable_places();
    two_concurrent_installs_end_with_one_consistent_registry_and_cache(self_path(argv[0]));
    expect_user_path_unchanged(&guard);
    return check_summary("install_ecosystem_test");
}
