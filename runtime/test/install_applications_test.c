#include "install_fixture.h"
#include "spisor/allocation.h"
#include "install_call.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "boot_stub.h"
#include "config.h"
#include "hosts.h"
#include "hosts_call.h"
#include "install_ecosystem.h"
#include "install_methods.h"
#include "install_support.h"
#include "registry.h"
#include "registry_call.h"

#define LOADER_ID "claude-loader"
#define BINARY "claude-binary"
#define PACKAGES "{\"fake\": \"claude-pkg\"}"

static void list_claude(install_base *base, const char *packages_json, const char *commands_json, const char *background_json) {
    list_foundation(base);
    list_loader(base, LOADER_ID, "1.0.0", application_json("claude", BINARY, packages_json), commands_json, 0,
                background_json);
    write_catalog(base);
}

/** The application binary on a directory of base's own, which a test puts on the PATH to have it detected. */
static char *install_binary(const install_base *base, const char *binary) {
    char *directory = joined(base->root, "bin");
    char name[256];
#if defined(_WIN32)
    snprintf(name, sizeof name, "%s.cmd", binary);
    write_in(directory, name, "@echo off\r\n");
#else
    snprintf(name, sizeof name, "%s", binary);
    char *path = joined(directory, name);
    write_file(path, "#!/bin/sh\n");
    CHECK(chmod(path, 0755) == 0);
    spisor_memory_release(path);
#endif
    return directory;
}

static void make_directory_in_user(const install_base *base, const char *name) {
    char *directory = joined(base->user, name);
    CHECK(spisor_make_directories(directory) == SPISOR_OK);
    spisor_memory_release(directory);
}

static cJSON *install_choosing(const install_base *base, const char *choices_json, const char *path) {
    char argument[512];
    snprintf(argument, sizeof argument, "{\"choices\": %s}", choices_json);
    return expect_report(install_with(base, argument, environment_with_path(base, path == NULL ? base->path : path)),
                         "install");
}

static const cJSON *application_row(const cJSON *report, const char *id) {
    for (const cJSON *row = cJSON_GetObjectItemCaseSensitive(report, "applications")->child; row != NULL; row = row->next) {
        if (strcmp(text_at(row, "id"), id) == 0) {
            return row;
        }
    }
    return NULL;
}

static cJSON *registration_of(const install_base *base, const char *id) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddStringToObject(argument, "id", id);
    registry_result result = registry_call(REGISTRY_GET, argument);
    cJSON *registration = cJSON_DetachItemFromObjectCaseSensitive(result.tree, "registration");
    cJSON_Delete(result.tree);
    return registration;
}

static cJSON *recorded_provenance(const install_base *base) {
    cJSON *registration = registration_of(base, LOADER_ID);
    cJSON *provenance = cJSON_DetachItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"),
                                                               "provenance");
    cJSON_Delete(registration);
    return provenance;
}

/** The version the fake method holds for claude-pkg, or NULL when it holds none. */
static char *stored_version(const install_base *base) {
    cJSON *store = json_in(base->cache, "methods/fake/installed.json");
    const char *version = text_at(store, "claude-pkg");
    char *copy = version == NULL ? NULL : spisor_memory_allocate(strlen(version) + 1);
    if (copy != NULL) {
        strcpy(copy, version);
    }
    cJSON_Delete(store);
    return copy;
}

static void remove_host_id(const install_base *base, const char *id) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddStringToObject(argument, "id", id);
    catalog_answer answer = hosts_call(HOSTS_REMOVE, argument);
    CHECK(answer.status == SPISOR_OK);
    cJSON_Delete(answer.tree);
}

static catalog_answer through_record(const install_base *base, const char *operation) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddStringToObject(argument, "loader", LOADER_ID);
    cJSON_AddStringToObject(argument, "operation", operation);
    return install_call(INSTALL_APPLICATION, argument);
}

static void asks_nothing_installs_nothing_and_reports_the_exact_flag(void) {
    install_base base;
    start_install_base(&base, "applications", "flag");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON *report = install_choosing(&base, "{}", NULL);
    expect_json(application_row(report, "claude"),
                "{\"id\":\"claude\",\"state\":\"missing\",\"offered\":[\"fake\"],\"flag\":\"--application claude=fake\"}",
                "missing row");
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "missing"), "[{\"application\":\"claude\",\"methods\":[\"fake\"]}]",
                "missing list");
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "loaders"), "[]", "no loader");
    char *version = stored_version(&base);
    CHECK(version == NULL);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_through_the_method_a_choice_names(void) {
    install_base base;
    start_install_base(&base, "applications", "choice");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON *report = install_choosing(&base, "{\"claude\": \"fake\"}", NULL);
    expect_json(application_row(report, "claude"),
                "{\"id\":\"claude\",\"state\":\"installed\",\"installedThrough\":\"fake\",\"origin\":{\"method\":\"fake\","
                "\"package\":\"claude-pkg\",\"version\":\"1.0.0\"}}",
                "installed row");
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "missing"), "[]", "nothing missing");
    char *version = stored_version(&base);
    check_text(version, "1.0.0", "fake store");
    spisor_memory_release(version);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_nothing_when_the_choice_is_skip(void) {
    install_base base;
    start_install_base(&base, "applications", "skip");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON *report = install_choosing(&base, "{\"claude\": \"skip\"}", NULL);
    expect_json(application_row(report, "claude"), "{\"id\":\"claude\",\"state\":\"skipped\"}", "skipped row");
    CHECK(stored_version(&base) == NULL);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void reports_a_method_unavailable_with_the_reason_when_none_is_built_in(void) {
    install_base base;
    start_install_base(&base, "applications", "unavailable");
    list_claude(&base, "{\"npm\": \"claude-pkg\"}", "[]", "[]");
    cJSON *report = install_choosing(&base, "{}", NULL);
    const cJSON *row = application_row(report, "claude");
    check_text(text_at(row, "state"), "unavailable", "unavailable state");
    const cJSON *reason = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(row, "unavailable"), 0);
    check_text(text_at(reason, "method"), "npm", "unavailable method");
    CHECK(strstr(text_at(reason, "message"), "npm") != NULL && text_at(reason, "fix") != NULL);
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "missing"), "[]", "nothing to ask");
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void refuses_a_choice_naming_a_method_that_is_not_offered_listing_the_ones_that_are(void) {
    install_base base;
    start_install_base(&base, "applications", "not-offered");
    list_claude(&base, PACKAGES, "[]", "[]");
    catalog_answer answer = install_with(&base, "{\"choices\": {\"claude\": \"winget\"}}", NULL);
    expect_cache_refusal_refused(answer, "claude=winget", "not offered");
    CHECK(strstr(text_at(answer.tree, "message"), "fake, skip") != NULL);
    CHECK(!exists_in(base.cache, "bundles"));
    cJSON_Delete(answer.tree);
    finish_install_base(&base);
}

static void refuses_a_choice_naming_an_application_the_catalog_does_not_list(void) {
    install_base base;
    start_install_base(&base, "applications", "unknown");
    list_claude(&base, PACKAGES, "[]", "[]");
    catalog_answer answer = install_with(&base, "{\"choices\": {\"cursor\": \"fake\"}}", NULL);
    expect_cache_refusal_refused(answer, "cursor", "unknown application");
    CHECK(strstr(text_at(answer.tree, "message"), "claude") != NULL);
    cJSON_Delete(answer.tree);
    finish_install_base(&base);
}

static void registers_one_loader_per_installed_or_detected_application_with_its_own_home(void) {
    install_base base;
    start_install_base(&base, "applications", "loaders");
    list_foundation(&base);
    list_loader(&base, "claude-loader", "1.0.0", application_json("claude", "claude", "{}"), "[]", 0, "[]");
    list_loader(&base, "opencode-loader", "1.0.0", application_json("opencode", "opencode", "{}"), "[]", 0, "[]");
    write_catalog(&base);
    char *bin = install_binary(&base, "claude");
    char *opencode = install_binary(&base, "opencode");
    cJSON *report = install_choosing(&base, "{}", bin);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(report, "loaders")) == 2);
    const char *ids[2] = {"claude-loader", "opencode-loader"};
    const char *applications[2] = {"claude", "opencode"};
    for (int index = 0; index < 2; index++) {
        cJSON *registration = registration_of(&base, ids[index]);
        char home_rest[128];
        snprintf(home_rest, sizeof home_rest, "homes/%s", applications[index]);
        char *home = joined(base.user, home_rest);
        check_text(text_at(registration, "home"), home, "loader home");
        check_text(text_at(cJSON_GetObjectItemCaseSensitive(registration, "provenance"), "method"), "catalog", "catalog provenance");
        spisor_memory_release(home);
        cJSON_Delete(registration);
    }
    spisor_memory_release(opencode);
    spisor_memory_release(bin);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void records_the_registered_version_of_each_loader_keeping_what_it_scheduled(void) {
    install_base base;
    start_install_base(&base, "applications", "loader-references");
    list_claude(&base, "{}", "[]", "[]");
    write_in(base.state, "bundles/" LOADER_ID ".json", "{\"run\": {}, \"next\": {\"" LOADER_ID "\": \"2.0.0\"}}");
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    cJSON *references = json_in(base.state, "bundles/" LOADER_ID ".json");
    expect_json(references, "{\"run\":{\"" LOADER_ID "\":\"1.0.0\"},\"next\":{\"" LOADER_ID "\":\"2.0.0\"}}", "loader references");
    cJSON_Delete(references);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

/** The settings the loader's own home and the portable place merge to, as a launch of the loader reads them. */
static cJSON *loader_settings(const install_base *base) {
    char *home = joined(base->user, "homes/claude");
    cJSON *refusal = NULL;
    cJSON *values = runtime_ecosystem_settings_values(home, base->portable, &refusal);
    CHECK(values != NULL && refusal == NULL);
    cJSON_Delete(refusal);
    spisor_memory_release(home);
    return values;
}

static void list_claude_requiring(install_base *base, const char *plugins_json) {
    list_foundation(base);
    list_loader_requiring(base, LOADER_ID, "1.0.0", application_json("claude", BINARY, "{}"), "[]", 0, "[]", plugins_json);
}

static void installs_and_enables_each_plugin_a_loader_requires_in_the_loader_s_home(void) {
    install_base base;
    start_install_base(&base, "applications", "required-plugins");
    list_plugin(&base, "plugin-stub", "1.0.0", "fixture-one");
    list_claude_requiring(&base, "[\"plugin-stub\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON *report = install_choosing(&base, "{}", bin);
    CHECK(exists_in(base.cache, "bundles/plugin-stub/1.0.0"));
    cJSON *installed = installed_ids(report);
    CHECK(runtime_install_holds(installed, "plugin-stub"));
    cJSON_Delete(installed);
    CHECK(runtime_install_holds(cJSON_GetObjectItemCaseSensitive(report, "enabled"), "plugin-stub"));
    cJSON *settings = loader_settings(&base);
    expect_json(cJSON_GetObjectItemCaseSensitive(settings, "plugins"), "{\"plugin-stub\":{\"enabled\":true}}",
                "the plugin enabled in the loader's home");
    cJSON *references = json_in(base.state, "bundles/" LOADER_ID ".json");
    expect_json(cJSON_GetObjectItemCaseSensitive(references, "run"), "{\"plugin-stub\":\"1.0.0\",\"" LOADER_ID "\":\"1.0.0\"}",
                "the loader references its plugin and itself");
    cJSON_Delete(references);
    cJSON *registration = registration_of(&base, LOADER_ID);
    CHECK(registration != NULL);
    cJSON_Delete(registration);
    cJSON_Delete(settings);
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void set_loader_setting(const install_base *base, const char *content) {
    char *home = joined(base->user, "homes/claude");
    write_in(home, "config/settings.json", content);
    spisor_memory_release(home);
}

static void leaves_a_required_plugin_the_user_disabled_disabled_and_reports_it(void) {
    install_base base;
    start_install_base(&base, "applications", "required-disabled");
    list_plugin(&base, "plugin-stub", "1.0.0", "fixture-one");
    list_claude_requiring(&base, "[\"plugin-stub\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    set_loader_setting(&base, "{\"plugins\": {\"plugin-stub\": {\"enabled\": false}}}");
    cJSON *report = install_choosing(&base, "{}", bin);
    cJSON *settings = loader_settings(&base);
    expect_json(cJSON_GetObjectItemCaseSensitive(settings, "plugins"), "{\"plugin-stub\":{\"enabled\":false}}",
                "the user's choice kept");
    CHECK(!runtime_install_holds(cJSON_GetObjectItemCaseSensitive(report, "enabled"), "plugin-stub"));
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(row, "id"), "plugin-stub", "the disabled plugin reported");
    CHECK(strstr(text_at(row, "fix"), "set plugins.plugin-stub.enabled to true in ") != NULL);
    CHECK(strstr(text_at(row, "fix"), "settings.json") != NULL);
    cJSON_Delete(settings);
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void names_the_portable_settings_file_when_the_portable_layer_disables_a_required_plugin(void) {
    install_base base;
    start_install_base(&base, "applications", "required-disabled-portable");
    list_plugin(&base, "plugin-stub", "1.0.0", "fixture-one");
    list_claude_requiring(&base, "[\"plugin-stub\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    set_loader_setting(&base, "{}");
    set_portable(&base, "plugins.plugin-stub.enabled", cJSON_CreateFalse());
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(row, "id"), "plugin-stub", "the disabled plugin reported");
    char *file = joined(base.portable, "settings.json");
    char fix[2048];
    snprintf(fix, sizeof fix, "set plugins.plugin-stub.enabled to true in %s", file);
    check_text(text_at(row, "fix"), fix, "the fix names the portable settings file that holds the false");
    spisor_memory_release(file);
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void reports_a_required_plugin_this_machine_cannot_install_naming_it(void) {
    install_base base;
    start_install_base(&base, "applications", "required-foreign");
    list_foreign_plugin(&base, "plugin-stub", "1.0.0");
    list_claude_requiring(&base, "[\"plugin-stub\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(row, "id"), "plugin-stub", "the plugin reported");
    cJSON *settings = loader_settings(&base);
    CHECK(cJSON_GetObjectItemCaseSensitive(settings, "plugins") == NULL ||
          !cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(settings, "plugins"), "plugin-stub"));
    cJSON_Delete(settings);
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void refuses_a_loader_whose_required_plugin_no_catalog_lists_without_a_row_for_the_plugin(void) {
    install_base base;
    start_install_base(&base, "applications", "required-absent");
    list_claude_requiring(&base, "[\"plugin-stub\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *unavailable = cJSON_GetObjectItemCaseSensitive(report, "unavailable");
    CHECK(cJSON_GetArraySize(unavailable) == 1);
    const cJSON *row = cJSON_GetArrayItem(unavailable, 0);
    check_text(text_at(row, "id"), LOADER_ID, "the loader's install refused");
    CHECK(strstr(text_at(row, "message"), "plugin-stub") != NULL);
    CHECK(strstr(text_at(row, "fix"), "catalog") != NULL);
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "loaders"), "[]", "no loader row");
    cJSON *registration = registration_of(&base, LOADER_ID);
    CHECK(registration == NULL);
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void registers_nothing_for_an_application_neither_installed_nor_detected(void) {
    install_base base;
    start_install_base(&base, "applications", "absent");
    list_claude(&base, "{}", "[]", "[]");
    cJSON *report = install_choosing(&base, "{}", NULL);
    expect_json(cJSON_GetObjectItemCaseSensitive(report, "loaders"), "[]", "no loaders");
    cJSON *registration = registration_of(&base, LOADER_ID);
    CHECK(registration == NULL);
    CHECK(!exists_in(base.cache, "bundles/" LOADER_ID));
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void installs_and_registers_each_background_host_in_a_home_of_its_own(void) {
    install_base base;
    start_install_base(&base, "applications", "background");
    list_host(&base, "keeper", "1.0.0", "native", "{\"entry\": \"keeper\"}");
    list_claude(&base, "{}", "[]", "[\"keeper\"]");
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    cJSON *registration = registration_of(&base, "keeper");
    char *home = joined(base.state, "home/keeper");
    check_text(text_at(registration, "home"), home, "background home");
    expect_json(cJSON_GetObjectItemCaseSensitive(registration, "start"), "{\"runtime\":\"native\",\"entry\":\"keeper\"}",
                "recorded start");
    cJSON *references = json_in(base.state, "bundles/keeper.json");
    expect_json(cJSON_GetObjectItemCaseSensitive(references, "run"), "{\"keeper\":\"1.0.0\"}", "background reference");
    cJSON_Delete(references);
    spisor_memory_release(home);
    cJSON_Delete(registration);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void reports_a_background_host_the_catalog_does_not_list_and_still_registers_its_loader(void) {
    install_base base;
    start_install_base(&base, "applications", "background-absent");
    list_claude(&base, "{}", "[]", "[\"ghost\"]");
    char *bin = install_binary(&base, BINARY);
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "unavailable"), 0);
    check_text(text_at(row, "id"), "ghost", "ghost reported");
    cJSON *registration = registration_of(&base, LOADER_ID);
    CHECK(registration != NULL);
    cJSON_Delete(registration);
    spisor_memory_release(bin);
    cJSON_Delete(report);
    finish_install_base(&base);
}

/**
 * The in-process entry of a loader requiring `required_json`, listed beside typescript-host, the TypeScript plugin
 * `scripted` and the fixture-one plugin `compiled`, with the plugins `enabled` names, a NULL-ended list, enabled in the portable place.
 */
static cJSON *install_in_process_loader(install_base *base, const char *label, const char *required_json,
                                        const char *const *enabled) {
    start_install_base(base, "applications", label);
    list_foundation(base);
    list_language_host(base, "typescript", "javascript", "1.0.0", "{}");
    list_plugin(base, "scripted", "1.0.0", "typescript");
    list_plugin(base, "compiled", "1.0.0", "fixture-one");
    for (const char *const *id = enabled; *id != NULL; id++) {
        char key[128];
        snprintf(key, sizeof key, "plugins.%s.enabled", *id);
        set_portable(base, key, cJSON_CreateTrue());
    }
    cJSON *application = application_json("sample", "sample", "{}");
    cJSON_AddItemToObject(application, "entry",
                          parsed("{\"runsInProcess\": true, \"directories\": [\"{{home}}\"],"
                                 " \"files\": [{\"path\": \"{{home}}/entry.js\", \"text\": \"{{hint}}\"}], \"settings\": []}"));
    list_loader_requiring(base, "sample-loader", "1.0.0", application, "[]", 0, "[]", required_json);
    write_catalog(base);
    char *bin = install_binary(base, "sample");
    make_directory_in_user(base, "homes");
    cJSON *report = install_choosing(base, "{}", bin);
    spisor_memory_release(bin);
    return report;
}

/** An in-process entry runs its front door's layer, so that language's host comes with the plugin holding it. */
static void brings_the_front_doors_language_host_for_an_in_process_entry(void) {
    install_base base;
    static const char *const nothing[1] = {NULL};
    cJSON *report = install_in_process_loader(&base, "in-process-door", "[\"scripted\"]", nothing);
    CHECK(exists_in(base.cache, "bundles/typescript-host"));
    const cJSON *entry = value_at(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "loaders"), 0), "entry");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "written")) &&
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "runsInProcess")));
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** A required plugin whose language no catalog source hosts is reported and left, as an enabled one is. */
static void reports_a_required_plugin_whose_language_no_catalog_source_hosts(void) {
    install_base base;
    start_install_base(&base, "applications", "required-unhosted");
    list_foundation(&base);
    list_plugin(&base, "spoken", "1.0.0", "lua");
    list_loader_requiring(&base, "sample-loader", "1.0.0", application_json("sample", "sample", "{}"), "[]", 0, "[]",
                          "[\"spoken\"]");
    write_catalog(&base);
    char *bin = install_binary(&base, "sample");
    make_directory_in_user(&base, "homes");
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *row = NULL;
    const cJSON *each;
    cJSON_ArrayForEach(each, cJSON_GetObjectItemCaseSensitive(report, "unavailable")) {
        row = text_at(each, "id") != NULL && strcmp(text_at(each, "id"), "spoken") == 0 ? each : row;
    }
    check_text(text_at(row, "message"),
               "plugins written in lua have no host: no catalog source lists lua-host, the language-host item that "
               "runs them",
               "the required plugin's language named");
    CHECK(!exists_in(base.cache, "bundles/spoken"));
    cJSON_Delete(report);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static const cJSON *entry_of_first_loader(const cJSON *report) {
    return value_at(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "loaders"), 0), "entry");
}

/** A loader requiring no plugin has its front door in the one language its plugins are written in, as a start reads it. */
static void writes_the_in_process_entry_of_a_loader_requiring_no_plugin_from_its_plugins_language(void) {
    static const char *const scripted[2] = {"scripted", NULL};
    install_base base;
    cJSON *report = install_in_process_loader(&base, "in-process-one-language", "[]", scripted);
    CHECK(exists_in(base.cache, "bundles/typescript-host"));
    const cJSON *entry = entry_of_first_loader(report);
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "written"))) {
        const char *message = text_at(entry, "message");
        fprintf(stderr, "the one-language entry: not written, \"%s\"\n", message == NULL ? "(none)" : message);
        check_failures++;
    }
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "runsInProcess")));
    cJSON_Delete(report);
    finish_install_base(&base);
}

/** With no plugin required and plugins of two languages, no language holds the front door, so no entry is written. */
static void writes_no_entry_for_a_loader_requiring_no_plugin_over_two_languages(void) {
    static const char *const both[3] = {"scripted", "compiled", NULL};
    install_base base;
    cJSON *report = install_in_process_loader(&base, "in-process-two-languages", "[]", both);
    const cJSON *entry = entry_of_first_loader(report);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry, "written")));
    const char *message = text_at(entry, "message");
    CHECK(message != NULL && strstr(message, "so no one language holds its front door") != NULL &&
          strstr(message, "typescript") != NULL && strstr(message, "fixture-one") != NULL);
    check_text(text_at(entry, "fix"),
               "require the plugin that holds the front door in the loader's standard, or enable plugins of one "
               "language only, then install the loader again",
               "the two languages' fix");
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void writes_each_entry_reports_one_without_and_removing_the_loader_takes_it_away(void) {
    install_base base;
    start_install_base(&base, "applications", "entries");
    list_foundation(&base);
    list_loader(&base, "claude-loader", "1.0.0", application_with_hook_entry("claude", "claude", "{}"), "[]", 0, "[]");
    list_loader(&base, "elsewhere-loader", "1.0.0", application_json("elsewhere", "claude", "{}"), "[]", 0, "[]");
    write_catalog(&base);
    char *bin = install_binary(&base, "claude");
    make_directory_in_user(&base, "homes");
    char *before = text_in(base.user, ".profile");
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *loaders = cJSON_GetObjectItemCaseSensitive(report, "loaders");
    CHECK(cJSON_GetArraySize(loaders) == 2);
    for (const cJSON *loader = loaders->child; loader != NULL; loader = loader->next) {
        const cJSON *entry = cJSON_GetObjectItemCaseSensitive(loader, "entry");
        if (strcmp(text_at(loader, "id"), "claude-loader") == 0) {
            expect_json(entry, "{\"written\":true}", "claude entry written");
        } else {
            CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry, "written")) && text_at(entry, "message") != NULL);
        }
    }
    CHECK(exists_in(base.user, "homes/claude/settings.json") && exists_in(base.state, "entries/claude-loader"));
    remove_host_id(&base, "claude-loader");
    CHECK(!exists_in(base.user, "homes/claude") && !exists_in(base.state, "entries"));
    spisor_memory_release(before);
    spisor_memory_release(bin);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void keeps_the_first_install_s_provenance_on_a_second_install(void) {
    install_base base;
    start_install_base(&base, "applications", "first-provenance");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    cJSON *first = recorded_provenance(&base);
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    cJSON *second = recorded_provenance(&base);
    CHECK(first != NULL && second != NULL && cJSON_Compare(first, second, 1));
    spisor_memory_release(bin);
    cJSON_Delete(second);
    cJSON_Delete(first);
    finish_install_base(&base);
}

static void requests_each_command_running_spisor_boot_with_launch(void) {
    install_base base;
    start_install_base(&base, "applications", "commands");
    list_claude(&base, "{}", "[\"cc\"]", "[]");
    char *bin = install_binary(&base, BINARY);
    cJSON *report = install_choosing(&base, "{}", bin);
    const cJSON *loader = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(report, "loaders"), 0);
    expect_json(cJSON_GetObjectItemCaseSensitive(loader, "commands"), "[\"cc\"]", "command held");
    expect_json(cJSON_GetObjectItemCaseSensitive(loader, "refusedCommands"), "[]", "none refused");
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(base.state, LOADER_ID, &registration) == RUNTIME_REGISTRY_OK);
    char *shim = NULL;
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(registration, "changes");
    for (const cJSON *recorded = changes == NULL ? NULL : changes->child; shim == NULL && recorded != NULL;
         recorded = recorded->next) {
        const char *path = text_at(recorded, "path");
        size_t length = path == NULL ? 0 : strlen(path);
        int is_shim = length > 3 && (strcmp(path + length - 3, ".cc") == 0 || strcmp(path + length - 3, "/cc") == 0 ||
                                     (length > 7 && strcmp(path + length - 7, "cc.shim") == 0));
        shim = is_shim ? text_in(path, "") : NULL;
    }
    char *boot = runtime_boot_stub_path(base.cache);
    CHECK(shim != NULL && strstr(shim, boot) != NULL && strstr(shim, "spisor-host") != NULL &&
          strstr(shim, "launch") != NULL && strstr(shim, LOADER_ID) != NULL);
    spisor_memory_release(boot);
    spisor_memory_release(shim);
    cJSON_Delete(registration);
    remove_host_id(&base, LOADER_ID);
    spisor_memory_release(bin);
    cJSON_Delete(report);
    finish_install_base(&base);
}

static void records_one_shape_for_an_application_installed_through_a_method_and_found_through_it(void) {
    install_base base;
    start_install_base(&base, "applications", "same-shape");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    cJSON *installed = recorded_provenance(&base);
    remove_host_id(&base, LOADER_ID);
    cJSON_Delete(install_choosing(&base, "{}", NULL));
    cJSON *found = recorded_provenance(&base);
    check_text(text_at(installed, "method"), "fake", "installed method");
    check_text(text_at(installed, "package"), "claude-pkg", "installed package");
    check_text(text_at(installed, "version"), "1.0.0", "installed version");
    check_text(text_at(found, "method"), "fake", "found method");
    check_text(text_at(found, "version"), "1.0.0", "found version");
    CHECK(cJSON_GetArraySize(installed) == cJSON_GetArraySize(found));
    for (const cJSON *member = installed == NULL ? NULL : installed->child; member != NULL; member = member->next) {
        CHECK(cJSON_HasObjectItem(found, member->string));
    }
    CHECK(text_at(installed, "installedAt") != NULL && text_at(installed, "location") != NULL);
    cJSON_Delete(found);
    cJSON_Delete(installed);
    finish_install_base(&base);
}

static void records_method_path_and_the_binary_s_path_for_an_application_found_only_on_path(void) {
    install_base base;
    start_install_base(&base, "applications", "path-only");
    list_claude(&base, "{}", "[]", "[]");
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    cJSON *recorded = recorded_provenance(&base);
    check_text(text_at(recorded, "method"), "path", "path method");
    CHECK(cJSON_GetObjectItemCaseSensitive(recorded, "package") == NULL);
    const char *location = text_at(recorded, "location");
    CHECK(location != NULL && strncmp(location, bin, strlen(bin)) == 0);
    cJSON_Delete(recorded);
    spisor_memory_release(bin);
    finish_install_base(&base);
}

static void updates_through_the_recorded_method_and_refreshes_the_recorded_version(void) {
    install_base base;
    start_install_base(&base, "applications", "update");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    catalog_answer answer = through_record(&base, "update");
    expect_json(answer.tree, "{\"operation\":\"update\",\"version\":\"2.0.0\"}", "update answer");
    cJSON_Delete(answer.tree);
    char *version = stored_version(&base);
    check_text(version, "2.0.0", "updated store");
    spisor_memory_release(version);
    cJSON *provenance = recorded_provenance(&base);
    check_text(text_at(provenance, "version"), "2.0.0", "recorded version");
    cJSON_Delete(provenance);
    finish_install_base(&base);
}

static void removes_through_the_recorded_method_and_leaves_the_loader_registered(void) {
    install_base base;
    start_install_base(&base, "applications", "remove");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    catalog_answer answer = through_record(&base, "remove");
    expect_json(answer.tree, "{\"operation\":\"remove\"}", "remove answer");
    cJSON_Delete(answer.tree);
    CHECK(stored_version(&base) == NULL);
    cJSON *registration = registration_of(&base, LOADER_ID);
    CHECK(registration != NULL);
    cJSON_Delete(registration);
    finish_install_base(&base);
}

static void refuses_an_update_and_a_remove_of_a_path_application_naming_where_it_came_from(void) {
    install_base base;
    start_install_base(&base, "applications", "path-refused");
    list_claude(&base, "{}", "[]", "[]");
    char *bin = install_binary(&base, BINARY);
    cJSON_Delete(install_choosing(&base, "{}", bin));
    const char *operations[2] = {"update", "remove"};
    for (int index = 0; index < 2; index++) {
        catalog_answer answer = through_record(&base, operations[index]);
        expect_cache_refusal_refused(answer, bin, operations[index]);
        CHECK(text_at(answer.tree, "fix") != NULL && strstr(text_at(answer.tree, "fix"), "whatever installed it") != NULL);
        cJSON_Delete(answer.tree);
    }
    spisor_memory_release(bin);
    finish_install_base(&base);
}

/** Makes the stored registration one registry.register refuses while registry.get still answers it. */
static void make_registration_unrewritable(const install_base *base) {
    char *path = joined(base->state, "hosts/" LOADER_ID ".json");
    cJSON *registration = json_in(base->state, "hosts/" LOADER_ID ".json");
    cJSON_ReplaceItemInObjectCaseSensitive(registration, "requiresCommand", cJSON_CreateString("yes"));
    char *text = cJSON_PrintUnformatted(registration);
    write_file(path, text);
    cJSON_free(text);
    cJSON_Delete(registration);
    spisor_memory_release(path);
}

static void reports_an_update_done_noting_a_version_that_could_not_be_recorded(void) {
    install_base base;
    start_install_base(&base, "applications", "update-unrecorded");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    make_registration_unrewritable(&base);
    catalog_answer answer = through_record(&base, "update");
    CHECK(answer.status == SPISOR_OK);
    check_text(text_at(answer.tree, "operation"), "update", "update done");
    check_text(text_at(answer.tree, "version"), "2.0.0", "version reported");
    const cJSON *unrecorded = cJSON_GetObjectItemCaseSensitive(answer.tree, "unrecorded");
    CHECK(text_at(unrecorded, "message") != NULL && text_at(unrecorded, "fix") != NULL);
    char *version = stored_version(&base);
    check_text(version, "2.0.0", "the application was updated");
    spisor_memory_release(version);
    cJSON_Delete(answer.tree);
    finish_install_base(&base);
}

static void never_removes_the_application_when_its_loader_is_removed(void) {
    install_base base;
    start_install_base(&base, "applications", "loader-removed");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    remove_host_id(&base, LOADER_ID);
    char *version = stored_version(&base);
    check_text(version, "1.0.0", "application kept");
    spisor_memory_release(version);
    finish_install_base(&base);
}

/* install.registerLoader, what a launch or a hook registers */

static catalog_answer register_loader(const install_base *base, const char *version, const char *path) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddItemToObject(argument, "environment", environment_with_path(base, path));
    cJSON_AddStringToObject(argument, "id", LOADER_ID);
    cJSON_AddStringToObject(argument, "version", version);
    return install_call(INSTALL_REGISTER_LOADER, argument);
}

static void registers_an_installed_loader_again_keeping_its_provenance_and_requesting_its_commands(void) {
    install_base base;
    start_install_base(&base, "applications", "register-again");
    list_claude(&base, PACKAGES, "[\"cc\"]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    cJSON *installed = recorded_provenance(&base);
    catalog_answer registered = register_loader(&base, "1.0.0", base.path);
    if (registered.status != SPISOR_OK) {
        char *printed = registered.tree == NULL ? NULL : cJSON_PrintUnformatted(registered.tree);
        fprintf(stderr, "install.registerLoader answered %d: %s\n", (int)registered.status, printed == NULL ? "(nothing)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    expect_json(cJSON_GetObjectItemCaseSensitive(registered.tree, "commands"), "[\"cc\"]", "commands requested again");
    cJSON *kept = recorded_provenance(&base);
    CHECK(installed != NULL && kept != NULL && cJSON_Compare(installed, kept, 1));
    cJSON *registration = registration_of(&base, LOADER_ID);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(registration, "application"), "id"), "claude", "application");
    cJSON_Delete(registration);
    cJSON_Delete(kept);
    cJSON_Delete(installed);
    cJSON_Delete(registered.tree);
    remove_host_id(&base, LOADER_ID);
    finish_install_base(&base);
}

static void refuses_to_register_a_loader_version_that_is_not_installed(void) {
    install_base base;
    start_install_base(&base, "applications", "register-absent");
    list_claude(&base, PACKAGES, "[]", "[]");
    cJSON_Delete(install_choosing(&base, "{\"claude\": \"fake\"}", NULL));
    catalog_answer refused = register_loader(&base, "9.9.9", base.path);
    CHECK(refused.status == SPISOR_REFUSED && text_at(refused.tree, "fix") != NULL);
    cJSON *registration = registration_of(&base, LOADER_ID);
    check_text(text_at(registration, "version"), "1.0.0", "registration untouched");
    cJSON_Delete(registration);
    cJSON_Delete(refused.tree);
    finish_install_base(&base);
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    asks_nothing_installs_nothing_and_reports_the_exact_flag();
    installs_through_the_method_a_choice_names();
    installs_nothing_when_the_choice_is_skip();
    reports_a_method_unavailable_with_the_reason_when_none_is_built_in();
    refuses_a_choice_naming_a_method_that_is_not_offered_listing_the_ones_that_are();
    refuses_a_choice_naming_an_application_the_catalog_does_not_list();
    registers_one_loader_per_installed_or_detected_application_with_its_own_home();
    records_the_registered_version_of_each_loader_keeping_what_it_scheduled();
    installs_and_enables_each_plugin_a_loader_requires_in_the_loader_s_home();
    refuses_a_loader_whose_required_plugin_no_catalog_lists_without_a_row_for_the_plugin();
    leaves_a_required_plugin_the_user_disabled_disabled_and_reports_it();
    names_the_portable_settings_file_when_the_portable_layer_disables_a_required_plugin();
    reports_a_required_plugin_this_machine_cannot_install_naming_it();
    registers_nothing_for_an_application_neither_installed_nor_detected();
    installs_and_registers_each_background_host_in_a_home_of_its_own();
    reports_a_background_host_the_catalog_does_not_list_and_still_registers_its_loader();
    writes_each_entry_reports_one_without_and_removing_the_loader_takes_it_away();
    brings_the_front_doors_language_host_for_an_in_process_entry();
    writes_the_in_process_entry_of_a_loader_requiring_no_plugin_from_its_plugins_language();
    writes_no_entry_for_a_loader_requiring_no_plugin_over_two_languages();
    reports_a_required_plugin_whose_language_no_catalog_source_hosts();
    keeps_the_first_install_s_provenance_on_a_second_install();
    requests_each_command_running_spisor_boot_with_launch();
    records_one_shape_for_an_application_installed_through_a_method_and_found_through_it();
    records_method_path_and_the_binary_s_path_for_an_application_found_only_on_path();
    updates_through_the_recorded_method_and_refreshes_the_recorded_version();
    removes_through_the_recorded_method_and_leaves_the_loader_registered();
    refuses_an_update_and_a_remove_of_a_path_application_naming_where_it_came_from();
    never_removes_the_application_when_its_loader_is_removed();
    reports_an_update_done_noting_a_version_that_could_not_be_recorded();
    registers_an_installed_loader_again_keeping_its_provenance_and_requesting_its_commands();
    refuses_to_register_a_loader_version_that_is_not_installed();
    expect_user_path_unchanged(&guard);
    return check_summary("install_applications_test");
}
