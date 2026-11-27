#include "install_fixture.h"
#include "spisor/allocation.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "boot.h"
#include "boot_call.h"
#include "boot_hint.h"
#include "hint_fixture.h"
#include "cache.h"
#include "catalog_time.h"
#include "child_process.h"
#include "host_references.h"
#include "hosts.h"
#include "hosts_call.h"
#include "install_hooks.h"
#include "install_support.h"
#include "process_run.h"
#include "registry.h"
#include "tree_snapshot.h"

#define PLUGINS "[\"announcer\", \"viewer\"]"

/** The catalog of the ecosystem tests: the foundation and two native plugins, plus a plugin built for another platform. */
static void list_ecosystem(install_base *base) {
    list_foundation(base);
    list_plugin(base, "announcer", "1.0.0", "fixture-one");
    list_plugin(base, "viewer", "1.0.0", "fixture-one");
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
    expect_installed_ids(report, "[\"announcer\",\"fixture-one-host\",\"forebay-library\",\"runtime\",\"spisor-host\",\"viewer\"]", "installed");
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
                  "{\"spisor-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\",\"viewer\":\"1.0.0\",\"forebay-library\":\"0.1.2\",\"fixture-one-host\":\"1.0.0\"}", "run");
    cJSON *hint = json_in(base.cache, "hosts/spisor-host.json");
    cJSON *versions = hinted_versions(hint);
    expect_json(versions, "{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"1.0.0\"}", "boot hint");
    cJSON_Delete(versions);
    cJSON_Delete(hint);
    cJSON_Delete(references);
    cJSON_free(source_json);
    cJSON_Delete(source);
    spisor_memory_release(index);
    cJSON_Delete(settings);
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** The file name a C library binary carries on this machine's platform. */
static const char *runtime_binary_name(void) {
    const char *platform = runtime_platform_key();
    if (strncmp(platform, "win32-", 6) == 0) {
        return "runtime.dll";
    }
    return strncmp(platform, "darwin-", 7) == 0 ? "libruntime.dylib" : "libruntime.so";
}

/** The runtime library as a C library ships: its library.json declaration and one binary, for this platform. */
static void list_declared_runtime(install_base *base) {
    cJSON *item = catalog_item(base, "runtime", "library", NULL, "0.1.0");
    list_file(base, item, "runtime", "0.1.0", RUNTIME_LIBRARY_DECLARATION, "{\"id\": \"runtime\"}", 1);
    cJSON *files = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(value_at(item, "versions/0.1.0/platforms"), runtime_platform_key()), "files");
    cJSON_AddItemToObject(files, runtime_binary_name(),
                          served_reference(base, "runtime", "0.1.0", runtime_binary_name(), "binary"));
}

static void records_the_binary_of_a_declared_c_library_in_the_boot_hint(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "declared");
    list_host(&base, "spisor-host", "1.0.0", "native", NULL);
    list_declared_runtime(&base);
    list_plugin(&base, "announcer", "1.0.0", "fixture-one");
    list_plugin(&base, "viewer", "1.0.0", "fixture-one");
    write_catalog(&base);
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *hint = json_in(base.cache, "hosts/spisor-host.json");
    const char *directory = text_at(value_at(hint, "libraries/runtime"), "directory");
    const char *binary = text_at(value_at(hint, "libraries/runtime"), "binary");
    char *expected = directory == NULL ? NULL : joined(directory, runtime_binary_name());
    check_text(binary, expected, "the hint names the declared library's binary");
    CHECK(binary != NULL && spisor_probe_file(binary) == SPISOR_FILE_PRESENT);
    spisor_memory_release(expected);
    cJSON_Delete(hint);
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
    expect_member(report, "cached", "[\"spisor-host@1.0.0\",\"runtime@0.1.0\",\"announcer@1.0.0\",\"viewer@1.0.0\",\"forebay-library@0.1.2\",\"fixture-one-host@1.0.0\"]",
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
    expect_installed_ids(report, "[\"announcer\",\"fixture-one-host\",\"forebay-library\",\"runtime\",\"spisor-host\",\"viewer\"]", "rest installed");
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
    expect_member(references, "run", "{\"spisor-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\",\"forebay-library\":\"0.1.2\",\"fixture-one-host\":\"1.0.0\"}",
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
    spisor_memory_release(first);
    cJSON_Delete(settings);
    cJSON_free(second_json);
    cJSON_Delete(quoted);
    spisor_memory_release(second);
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
    spisor_memory_release(location);
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
    list_plugin(base, "announcer", "1.0.0", "fixture-one");
    list_plugin(base, "viewer", "1.0.0", "fixture-one");
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
    catalog_answer placed = boot_call(BOOT_PLACE, argument);
    CHECK(placed.status == SPISOR_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(placed.tree, "placed")));
    char *stub = text_in(base.cache, "boot/spisor-boot" EXECUTABLE_SUFFIX);
    check_text(stub, "stub", "spisor-boot placed");
    cJSON *registration = json_in(base.state, "hosts/spisor-host.json");
    CHECK(registration != NULL);
    char *old_host = text_in(base.cache, OLD_HOST_FILE);
    char *old_stub = text_in(base.cache, OLD_STUB_FILE);
    check_text(old_host, OLD_FILE_CONTENT, "old host bundle untouched");
    check_text(old_stub, OLD_FILE_CONTENT, "old boot stub untouched");
    spisor_memory_release(old_stub);
    spisor_memory_release(old_host);
    cJSON_Delete(registration);
    spisor_memory_release(stub);
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

static void refuses_having_written_nothing_when_no_source_yields_a_catalog(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "no-catalog");
    char *before = tree_snapshot(base.root, "/cache");
    catalog_answer answer = install_with(&base, "{\"defaults\": " PLUGINS "}", NULL);
    char *after = tree_snapshot(base.root, "/cache");
    CHECK(answer.status == SPISOR_REFUSED);
    CHECK(strstr(text_at(answer.tree, "message") == NULL ? "" : text_at(answer.tree, "message"),
                 "no catalog source yields a catalog") != NULL);
    CHECK(strstr(text_at(answer.tree, "message") == NULL ? "" : text_at(answer.tree, "message"),
                 " could not be checked: the file could not be read") != NULL);
    check_text(after, before, "everything but the catalog cache after a refused install");
    CHECK(!exists_in(base.root, "state"));
    spisor_memory_release(after);
    spisor_memory_release(before);
    cJSON_Delete(answer.tree);
    finish_install_base(&base);
}

static void answers_malformed_for_a_source_that_is_neither_local_nor_https(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "bad-source");
    list_ecosystem(&base);
    catalog_answer answer = install_with(&base, "{\"sources\": [{\"kind\": \"ftp\", \"url\": \"ftp://x\"}]}", NULL);
    expect_cache_refusal_malformed(answer, "sources", "bad source kind");
    cJSON_Delete(answer.tree);
    answer = install_with(&base, "{\"sources\": [{\"kind\": \"local\"}]}", NULL);
    expect_cache_refusal_malformed(answer, "sources", "local source without a path");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "bundles"));
    finish_install_base(&base);
}

static void acquires_no_javascript_runtime_for_a_catalog_with_no_javascript_item(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "no-javascript");
    list_ecosystem(&base);
    char *manifest = typescript_host_json("1.2.3", "22.18.0", "file:///nowhere/node.zip",
                                          "0000000000000000000000000000000000000000000000000000000000000000");
    list_language_host(&base, "typescript", "javascript", "1.0.0", manifest);
    spisor_memory_release(manifest);
    write_catalog(&base);
    cJSON *report = install_defaults(&base, "");
    CHECK(cJSON_GetObjectItemCaseSensitive(report, "javascript") == NULL);
    expect_member(report, "unavailable", "[]", "nothing unavailable");
    CHECK(!exists_in(base.cache, "runtimes") && !exists_in(base.cache, "bundles/typescript-host"));
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** A served runtime build install.runtime is pointed at: its archive's file: url and SHA-256. */
typedef struct served_build {
    char url[2048];
    char sha256[NAULA_SHA256_HEX_SIZE];
} served_build;

/** Bytes that do not compress, so an archive of them takes long enough to fetch and unpack that two installs overlap. */
static void write_incompressible(const char *path, size_t size) {
    char *bytes = spisor_memory_allocate(size);
    unsigned long state = 2463534242UL;
    for (size_t index = 0; index < size; index++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = (char)(state & 0xff);
    }
    char *parent = joined(path, "..");
    CHECK(spisor_make_directories(parent) == SPISOR_OK);
    CHECK(spisor_replace_file(path, (const uint8_t *)bytes, size) == SPISOR_OK);
    spisor_memory_release(parent);
    spisor_memory_release(bytes);
}

/** Packs a one-file runtime archive of size bytes with the OS's tar, for typescript-host's host.json to pin. */
static void serve_runtime_build(install_base *base, size_t size, served_build *served) {
    char stem[64];
    char member[96];
    char archive_name[96];
    node_archive_stem("1.2.3", stem, sizeof stem);
#if defined(_WIN32)
    snprintf(member, sizeof member, "%s/node.exe", stem);
    snprintf(archive_name, sizeof archive_name, "%s.zip", stem);
#else
    snprintf(member, sizeof member, "%s/bin/node", stem);
    snprintf(archive_name, sizeof archive_name, "%s.tar.gz", stem);
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
    char *tar = joined(spisor_probe_file("/usr/bin/tar") == SPISOR_FILE_PRESENT ? "/usr/bin" : "/bin", "tar");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-czf"));
#endif
    cJSON_AddItemToArray(arguments, cJSON_CreateString(archive));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-C"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(packing));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(stem));
    int exit_code = -1;
    char *problem = NULL;
    CHECK(runtime_run_to_exit(tar, arguments, 120000, &exit_code, NULL, &problem) && exit_code == 0);
    CHECK(spisor_sha256_file(archive, served->sha256) == SPISOR_OK);
    char *url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, archive, &url);
    snprintf(served->url, sizeof served->url, "%s", url);
    spisor_memory_release(url);
    spisor_memory_release(problem);
    spisor_memory_release(tar);
    cJSON_Delete(arguments);
    spisor_memory_release(archive);
    spisor_memory_release(executable);
    spisor_memory_release(packing);
}

static void acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "javascript");
    list_foundation(&base);
    list_plugin(&base, "scripted", "1.0.0", "typescript");
    list_library(&base, "forebay-library", "0.1.2");
    served_build served;
    serve_runtime_build(&base, 64, &served);
    char *manifest = typescript_host_json("1.2.3", "22.18.0", served.url, served.sha256);
    list_language_host(&base, "typescript", "javascript", "1.0.0", manifest);
    spisor_memory_release(manifest);
    write_catalog(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"scripted\"]}", NULL), "javascript");
    expect_installed_ids(report, "[\"forebay-library\",\"runtime\",\"scripted\",\"spisor-host\",\"typescript-host\"]", "with its host");
    const cJSON *javascript = cJSON_GetObjectItemCaseSensitive(report, "javascript");
    CHECK(javascript != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(javascript, "fetched")) &&
          spisor_probe_file(text_at(javascript, "executable")) == SPISOR_FILE_PRESENT);
    check_text(text_at(javascript, "version"), "1.2.3", "the version typescript-host's host.json pins");
    cJSON *hint = json_in(base.cache, "hosts/typescript-host.json");
    cJSON *versions = hinted_versions(hint);
    expect_json(versions, "{\"libraries\":{\"runtime\":\"0.1.0\",\"forebay-library\":\"0.1.2\"},\"host\":\"1.0.0\"}", "typescript-host hint");
    cJSON_Delete(versions);
    cJSON_Delete(hint);
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** A plugin whose language has no host item in any catalog source cannot run, so install reports it and leaves it. */
static void reports_a_plugin_whose_language_no_catalog_source_hosts_and_installs_the_rest(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "unhosted");
    list_ecosystem(&base);
    list_plugin(&base, "spoken", "1.0.0", "lua");
    write_catalog(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"announcer\", \"spoken\"]}", NULL), "unhosted");
    expect_installed_ids(report, "[\"announcer\",\"fixture-one-host\",\"forebay-library\",\"runtime\",\"spisor-host\"]",
                         "the hosted plugin installed");
    const cJSON *unavailable = cJSON_GetObjectItemCaseSensitive(report, "unavailable");
    const cJSON *row = cJSON_GetArrayItem(unavailable, 0);
    CHECK(cJSON_GetArraySize(unavailable) == 1);
    check_text(text_at(row, "id"), "spoken", "the unhosted plugin reported");
    check_text(text_at(row, "message"),
               "plugins written in lua have no host: no catalog source lists lua-host, the language-host item that "
               "runs them",
               "the language named");
    check_text(text_at(row, "fix"), "add a catalog source listing lua-host, or disable the plugins written in lua",
               "the fix naming the language");
    CHECK(!exists_in(base.cache, "bundles/spoken") && !exists_in(base.cache, "bundles/lua-host"));
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** Lists the interface item id, native, its bundle's host.json naming its entry. */
static void list_interface(install_base *base, const char *id) {
    list_file(base, catalog_item(base, id, "interface", "native", "0.1.0"), id, "0.1.0", "host.json",
              "{\"entry\": \"interface\", \"runtime\": \"native\"}", 0);
}

static void installs_and_hints_the_catalogs_one_interface(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "interface");
    list_ecosystem(&base);
    list_interface(&base, "spisor-interface");
    write_catalog(&base);
    cJSON *report = install_defaults(&base, "");
    expect_installed_ids(report,
                         "[\"announcer\",\"fixture-one-host\",\"forebay-library\",\"runtime\",\"spisor-host\","
                         "\"spisor-interface\",\"viewer\"]",
                         "the interface installed with the ecosystem");
    cJSON *hint = json_in(base.cache, "hosts/spisor-interface.json");
    check_text(text_at(hint, "host"), "0.1.0", "the interface hinted at its version");
    CHECK(value_at(hint, "libraries/runtime") != NULL);
    cJSON_Delete(hint);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_no_interface_when_the_setting_says_none(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "headless");
    list_ecosystem(&base);
    list_interface(&base, "spisor-interface");
    write_catalog(&base);
    set_portable(&base, "interface", cJSON_CreateString("none"));
    cJSON *report = install_defaults(&base, "");
    CHECK(!exists_in(base.cache, "bundles/spisor-interface") && !exists_in(base.cache, "hosts/spisor-interface.json"));
    expect_member(report, "unavailable", "[]", "a headless install reports nothing");
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void reports_two_interfaces_none_chooses_naming_both_and_installs_neither(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "two-interfaces");
    list_ecosystem(&base);
    list_interface(&base, "spisor-interface");
    list_interface(&base, "other-interface");
    write_catalog(&base);
    cJSON *report = install_defaults(&base, "");
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(row, "id"), "interface", "the interface setting reported");
    const char *message = text_at(row, "message");
    CHECK(message != NULL && strstr(message, "spisor-interface") != NULL && strstr(message, "other-interface") != NULL);
    const char *fix = text_at(row, "fix");
    CHECK(fix != NULL && strstr(fix, "interface setting") != NULL && strstr(fix, "none") != NULL);
    CHECK(!exists_in(base.cache, "bundles/spisor-interface") && !exists_in(base.cache, "bundles/other-interface"));
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_the_interface_the_setting_chooses(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "chosen-interface");
    list_ecosystem(&base);
    list_interface(&base, "spisor-interface");
    list_interface(&base, "other-interface");
    write_catalog(&base);
    set_portable(&base, "interface", cJSON_CreateString("other-interface"));
    cJSON *report = install_defaults(&base, "");
    CHECK(exists_in(base.cache, "bundles/other-interface/0.1.0/host.json"));
    CHECK(!exists_in(base.cache, "bundles/spisor-interface"));
    expect_member(report, "unavailable", "[]", "nothing unavailable");
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_the_interface_and_no_host_item_when_no_plugin_is_enabled(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "no-plugin");
    list_foundation(&base);
    list_interface(&base, "spisor-interface");
    write_catalog(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": []}", NULL), "no plugin");
    expect_installed_ids(report, "[\"runtime\",\"spisor-host\",\"spisor-interface\"]", "the interface alone");
    cJSON *hint = json_in(base.cache, "hosts/spisor-interface.json");
    check_text(text_at(hint, "host"), "0.1.0", "the interface hinted");
    CHECK(!exists_in(base.cache, "bundles/fixture-one-host") && !exists_in(base.cache, "hosts/fixture-one-host.json"));
    cJSON_Delete(hint);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_the_host_of_an_enabled_plugin_s_language_and_no_other(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "rust");
    list_foundation(&base);
    list_language_host(&base, "rust", "native", "1.0.0", "{\"entry\": \"weivel-host\", \"runtime\": \"native\"}");
    list_language_host(&base, "typescript", "javascript", "1.0.0",
                       "{\"entry\": \"weivel-host.mjs\", \"runtime\": \"javascript\"}");
    list_plugin(&base, "carved", "1.0.0", "rust");
    write_catalog(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"carved\"]}", NULL), "rust");
    expect_installed_ids(report, "[\"carved\",\"forebay-library\",\"runtime\",\"rust-host\",\"spisor-host\"]",
                         "the rust plugin and its host");
    cJSON *hint = json_in(base.cache, "hosts/rust-host.json");
    check_text(text_at(hint, "host"), "1.0.0", "rust-host hinted");
    CHECK(!exists_in(base.cache, "bundles/typescript-host") && !exists_in(base.cache, "hosts/typescript-host.json"));
    CHECK(!exists_in(base.cache, "bundles/fixture-one-host"));
    cJSON_Delete(hint);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void answers_malformed_for_an_argument_with_no_usable_places(void) {
    catalog_answer answer = install_call(INSTALL_ECOSYSTEM, parsed("{\"sources\": [], \"places\": {}, \"environment\": {}}"));
    expect_malformed_released(answer, "portable", "no places");
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

/** Frees what enter_install_base made, leaving the folders to the process that created them. */
static void leave_install_base(install_base *base) {
    spisor_memory_release(base->portable);
    spisor_memory_release(base->state);
    spisor_memory_release(base->cache);
    spisor_memory_release(base->user);
    spisor_memory_release(base->path);
    spisor_memory_release(base->catalog);
}

/** The child half of the concurrent test: one install requesting the comma-separated plugins. */
static int run_child_install(const char *root, const char *requested) {
    install_base base;
    enter_install_base(&base, root);
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
    int succeeded = answer.status == SPISOR_OK &&
                    cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answer.tree, "unavailable")) == 0;
    if (!succeeded) {
        char *printed = answer.tree == NULL ? NULL : cJSON_PrintUnformatted(answer.tree);
        fprintf(stderr, "child install: %s\n", printed == NULL ? "(nothing)" : printed);
        cJSON_free(printed);
    }
    cJSON_Delete(answer.tree);
    leave_install_base(&base);
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
    spisor_memory_release(directory);
    catalog_answer answer = cache_call(CACHE_CHECK, argument);
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
        list_plugin(base, first, "1.0.0", "fixture-one");
        list_plugin(base, second, "1.0.0", "fixture-one");
    }
}

/** Whether the JavaScript runtime the installs acquired is unpacked intact: its marker names the executable's bytes. */
static int runtime_is_intact(const install_base *base) {
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s", runtime_platform_key());
    char *directory = joined(base->cache, rest);
    char stem[64];
    char member[96];
    node_archive_stem("1.2.3", stem, sizeof stem);
#if defined(_WIN32)
    snprintf(member, sizeof member, "%s/node.exe", stem);
#else
    snprintf(member, sizeof member, "%s/bin/node", stem);
#endif
    char *executable = joined(directory, member);
    char *marker = text_in(directory, ".unpacked");
    char hash[NAULA_SHA256_HEX_SIZE];
    int intact = marker != NULL && strlen(marker) == 130 && spisor_sha256_file(executable, hash) == SPISOR_OK &&
                 strncmp(marker + 65, hash, 64) == 0;
    spisor_memory_release(marker);
    spisor_memory_release(executable);
    spisor_memory_release(directory);
    return intact;
}

/** No acquisition left a staging folder behind in the runtime's version folder. */
static void expect_no_staging_left(const install_base *base) {
    char *versions = joined(base->cache, "runtimes/node/1.2.3");
    char prefix[64];
    snprintf(prefix, sizeof prefix, ".%s-staging-", runtime_platform_key());
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(versions, &listing) == SPISOR_OK);
    for (size_t index = 0; index < listing.count; index++) {
        if (strncmp(listing.entries[index].name, prefix, strlen(prefix)) == 0) {
            fprintf(stderr, "a staging folder was left: %s\n", listing.entries[index].name);
            check_failures++;
        }
    }
    runtime_free_directory_listing(&listing);
    spisor_memory_release(versions);
}

/** What runtimes/javascript.json records: the acquired pin at its final executable. */
static void expect_recorded_runtime(const install_base *base) {
    cJSON *record = json_in(base->cache, "runtimes/javascript.json");
    char rest[256];
    char stem[64];
    node_archive_stem("1.2.3", stem, sizeof stem);
#if defined(_WIN32)
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s/%s/node.exe", runtime_platform_key(), stem);
#else
    snprintf(rest, sizeof rest, "runtimes/node/1.2.3/%s/%s/bin/node", runtime_platform_key(), stem);
#endif
    char *executable = joined(base->cache, rest);
    check_text(text_at(record, "name"), "node", "recorded name");
    check_text(text_at(record, "version"), "1.2.3", "recorded version");
    check_text(text_at(record, "source"), "acquired", "recorded source");
    check_text(text_at(record, "executable"), executable, "recorded executable");
    CHECK(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(record, "fetched")));
    spisor_memory_release(executable);
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
    list_plugin(&base, "scripted", "1.0.0", "typescript");
    list_many_plugins(&base);
    list_library(&base, "forebay-library", "0.1.2");
    served_build served;
    serve_runtime_build(&base, 24 * 1024 * 1024, &served);
    char *manifest = typescript_host_json("1.2.3", "22.18.0", served.url, served.sha256);
    list_language_host(&base, "typescript", "javascript", "1.0.0", manifest);
    spisor_memory_release(manifest);
    cJSON *application = application_json("claude", "claude", "{}");
    list_loader(&base, "claude-loader", "1.0.0", application, "[]", 0, "[\"keeper\"]");
    list_host(&base, "keeper", "1.0.0", "native", NULL);
    write_catalog(&base);
    write_in(base.path, "claude.cmd", "@echo off\r\n");
#if !defined(_WIN32)
    char *binary = joined(base.path, "claude");
    write_file(binary, "#!/bin/sh\n");
    CHECK(chmod(binary, 0755) == 0);
    spisor_memory_release(binary);
#endif
    const char *first_arguments[3] = {"--child", base.root, "announcer,first-1,first-2,first-3,first-4,first-5,scripted"};
    const char *second_arguments[3] = {"--child", base.root, "viewer,second-1,second-2,second-3,second-4,second-5,scripted"};
    child_process first = start_child(self, first_arguments, 3, 0, NULL);
    child_process second = start_child(self, second_arguments, 3, 0, NULL);
    CHECK(first.pid != 0 && second.pid != 0);
    CHECK(child_exit_code(&first) == 0);
    CHECK(child_exit_code(&second) == 0);
    cJSON *listed = cJSON_CreateObject();
    cJSON_AddItemToObject(listed, "places", places_of(&base));
    catalog_answer hosts = hosts_call(HOSTS_LIST, listed);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hosts.tree, "hosts")) == 3);
    expect_json(cJSON_GetObjectItemCaseSensitive(hosts.tree, "unreadable"), "[]", "no unreadable registration");
    cJSON_Delete(hosts.tree);
    const char *versions[8] = {"bundles/spisor-host/1.0.0", "bundles/announcer/1.0.0", "bundles/viewer/1.0.0",
                               "bundles/scripted/1.0.0", "bundles/typescript-host/1.0.0", "bundles/claude-loader/1.0.0",
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
    if (argc == 4 && strcmp(argv[1], "--child") == 0) {
        return run_child_install(argv[2], argv[3]);
    }
    user_path_guard guard;
    guard_user_path(&guard);
    installs_the_host_the_runtime_and_the_defaults_and_records_them();
    records_the_binary_of_a_declared_c_library_in_the_boot_hint();
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
    refuses_having_written_nothing_when_no_source_yields_a_catalog();
    answers_malformed_for_a_source_that_is_neither_local_nor_https();
    acquires_no_javascript_runtime_for_a_catalog_with_no_javascript_item();
    reports_a_plugin_whose_language_no_catalog_source_hosts_and_installs_the_rest();
    installs_and_hints_the_catalogs_one_interface();
    installs_no_interface_when_the_setting_says_none();
    reports_two_interfaces_none_chooses_naming_both_and_installs_neither();
    installs_the_interface_the_setting_chooses();
    installs_the_interface_and_no_host_item_when_no_plugin_is_enabled();
    installs_the_host_of_an_enabled_plugin_s_language_and_no_other();
    acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin();
    answers_malformed_for_an_argument_with_no_usable_places();
    two_concurrent_installs_end_with_one_consistent_registry_and_cache(self_path(argv[0]));
    expect_user_path_unchanged(&guard);
    return check_summary("install_ecosystem_test");
}
