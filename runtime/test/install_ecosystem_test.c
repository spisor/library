#include "install_fixture.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include "boot_hint.h"
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
    expect_installed_ids(report, "[\"announcer\",\"forebay-host\",\"runtime\",\"viewer\"]", "installed");
    expect_member(report, "cached", "[]", "cached");
    expect_member(report, "unavailable", "[]", "unavailable");
    expect_member(report, "enabled", "[\"announcer\",\"viewer\"]", "enabled");
    CHECK(cJSON_GetObjectItemCaseSensitive(report, "javascript") == NULL);
    char library[256];
    snprintf(library, sizeof library, "libraries/runtime/0.1.0/%s/library.bin", runtime_platform_key());
    CHECK(exists_in(base.cache, library) && exists_in(base.cache, "bundles/forebay-host/1.0.0/host.json"));
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
    cJSON *references = json_in(base.state, "bundles/forebay-host.json");
    expect_member(references, "run",
                  "{\"forebay-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\",\"viewer\":\"1.0.0\"}", "run");
    cJSON *hint = json_in(base.cache, "hosts/forebay-host.json");
    expect_json(hint, "{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"1.0.0\"}", "boot hint");
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
    expect_member(report, "cached", "[\"forebay-host@1.0.0\",\"runtime@0.1.0\",\"announcer@1.0.0\",\"viewer@1.0.0\"]",
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
    expect_installed_ids(report, "[\"announcer\",\"forebay-host\",\"runtime\",\"viewer\"]", "rest installed");
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
    cJSON *hint = json_in(base.cache, "hosts/forebay-host.json");
    expect_member(hint, "libraries", "{\"runtime\":\"0.0.9\"}", "hinted runtime");
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
    write_in(base.state, "bundles/forebay-host.json", "{\"run\": {}, \"next\": {\"viewer\": \"2.0.0\"}}");
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *references = json_in(base.state, "bundles/forebay-host.json");
    expect_member(references, "next", "{\"viewer\":\"2.0.0\"}", "next kept");
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
    cJSON *registration = json_in(base.state, "hosts/forebay-host.json");
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(registration, "provenance");
    check_text(text_at(provenance, "method"), "catalog", "provenance method");
    check_text(text_at(provenance, "version"), "1.0.0", "provenance version");
    char *location = joined(base.cache, "bundles/forebay-host/1.0.0");
    check_text(text_at(provenance, "location"), location, "provenance location");
    double installed_at = 0;
    CHECK(runtime_catalog_parse_time(text_at(provenance, "installedAt"), &installed_at));
    CHECK(installed_at >= before - 1000 && installed_at <= runtime_now_milliseconds());
    cJSON_Delete(install_defaults(&base, ""));
    cJSON *again = json_in(base.state, "hosts/forebay-host.json");
    char *first = cJSON_PrintUnformatted(provenance);
    expect_json(cJSON_GetObjectItemCaseSensitive(again, "provenance"), first, "provenance kept");
    cJSON_free(first);
    cJSON_Delete(again);
    free(location);
    cJSON_Delete(registration);
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

/** Packs a one-file runtime archive with the OS's tar and makes install.runtime acquire it; answers its stem's executable path inside. */
static void serve_runtime_build(install_base *base) {
#if defined(_WIN32)
    const char *member = "node-v1.2.3-fixture/node.exe";
    const char *archive_name = "node-v1.2.3-fixture.zip";
#else
    const char *member = "node-v1.2.3-fixture/bin/node";
    const char *archive_name = "node-v1.2.3-fixture.tar.gz";
#endif
    char *packing = joined(base->root, "packing");
    write_in(packing, member, "the node executable");
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
    CHECK(runtime_run_to_exit(tar, arguments, &exit_code, &problem) && exit_code == 0);
    char sha256[RUNTIME_SHA256_HEX_SIZE];
    CHECK(spisor_sha256_file(archive, sha256));
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, archive);
    runtime_install_use_runtime_build("1.2.3", url, sha256);
    free(url);
    free(problem);
    free(tar);
    cJSON_Delete(arguments);
    free(archive);
    free(packing);
}

static void acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin(void) {
    install_base base;
    start_install_base(&base, "ecosystem", "javascript");
    list_foundation(&base);
    list_plugin(&base, "scripted", "1.0.0", NULL);
    list_library(&base, "koffi", "2.16.3");
    list_host(&base, "javascript-host", "1.0.0", "javascript", NULL);
    write_catalog(&base);
    serve_runtime_build(&base);
    cJSON *report = expect_report(install_with(&base, "{\"defaults\": [\"scripted\"]}", NULL), "javascript");
    expect_installed_ids(report, "[\"forebay-host\",\"javascript-host\",\"koffi\",\"runtime\",\"scripted\"]", "with its host");
    const cJSON *javascript = cJSON_GetObjectItemCaseSensitive(report, "javascript");
    CHECK(javascript != NULL && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(javascript, "fetched")) &&
          spisor_file_exists(text_at(javascript, "executable")));
    cJSON *hint = json_in(base.cache, "hosts/javascript-host.json");
    expect_json(hint, "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"2.16.3\"},\"host\":\"1.0.0\"}", "javascript-host hint");
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

static int run_child_install(const char *root) {
    install_base base;
    enter_install_base(&base, root);
    catalog_answer answer = install_with(&base, "{\"defaults\": " PLUGINS ", \"choices\": {}}", NULL);
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

/** Two installs at once over one base end with one consistent registry and cache, and neither fails (Review Focus 4). */
static void two_concurrent_installs_end_with_one_consistent_registry_and_cache(const char *self) {
    install_base base;
    start_install_base(&base, "ecosystem", "concurrent");
    list_ecosystem(&base);
    cJSON *application = application_json("claude", "claude", "{}");
    list_loader(&base, "claude-loader", "1.0.0", application, "[]", 0, "[\"keeper\"]");
    list_host(&base, "keeper", "1.0.0", "native", NULL);
    write_catalog(&base);
    write_in(base.path, "claude.cmd", "@echo off\r\n");
#if !defined(_WIN32)
    char *binary = joined(base.path, "claude");
    write_file(binary, "#!/bin/sh\n");
    CHECK(chmod(binary, 0755) == 0);
    free(binary);
#endif
    const char *arguments[2] = {"--child", base.root};
    child_process first = start_child(self, arguments, 2, 0, NULL);
    child_process second = start_child(self, arguments, 2, 0, NULL);
    CHECK(first.pid != 0 && second.pid != 0);
    CHECK(child_exit_code(&first) == 0);
    CHECK(child_exit_code(&second) == 0);
    cJSON *listed = cJSON_CreateObject();
    cJSON_AddItemToObject(listed, "places", places_of(&base));
    catalog_answer hosts = call_catalog(runtime_hosts_list, listed);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(hosts.tree, "hosts")) == 3);
    expect_json(cJSON_GetObjectItemCaseSensitive(hosts.tree, "unreadable"), "[]", "no unreadable registration");
    cJSON_Delete(hosts.tree);
    expect_intact(&base, "bundles/forebay-host/1.0.0");
    expect_intact(&base, "bundles/announcer/1.0.0");
    expect_intact(&base, "bundles/viewer/1.0.0");
    expect_intact(&base, "bundles/claude-loader/1.0.0");
    expect_intact(&base, "bundles/keeper/1.0.0");
    cJSON *references = json_in(base.state, "bundles/forebay-host.json");
    expect_member(references, "run",
                  "{\"forebay-host\":\"1.0.0\",\"runtime\":\"0.1.0\",\"announcer\":\"1.0.0\",\"viewer\":\"1.0.0\"}",
                  "one consistent run");
    cJSON *settings = ecosystem_settings(&base, 1, 1);
    expect_member(settings, "plugins", "{\"announcer\":{\"enabled\":true},\"viewer\":{\"enabled\":true}}", "plugins once");
    CHECK(cJSON_GetArraySize(value_at(settings, "catalog/sources")) == 1);
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
    if (argc == 3 && strcmp(argv[1], "--child") == 0) {
        return run_child_install(argv[2]);
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
    records_a_local_source_in_the_application_layer_never_the_portable_one();
    appends_a_source_not_yet_listed_and_never_replaces_the_ones_that_are();
    registers_the_host_at_install_keeping_its_provenance_on_a_later_install();
    acquires_no_javascript_runtime_for_a_catalog_with_no_javascript_item();
    acquires_the_javascript_runtime_and_its_host_for_a_javascript_plugin();
    refuses_an_argument_with_no_usable_places();
    two_concurrent_installs_end_with_one_consistent_registry_and_cache(self_path(argv[0]));
    expect_user_path_unchanged(&guard);
    return check_summary("install_ecosystem_test");
}
