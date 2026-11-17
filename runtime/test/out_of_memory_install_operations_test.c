#include "hosts_call.h"
#include "install_fixture.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "allocation.h"
#include "catalog_text.h"
#include "child_process.h"
#include "definition_fixture.h"
#include "directory_tree.h"
#include "hosts.h"
#include "install_hooks.h"
#include "json_allocation.h"
#include "file_times.h"
#include "platform_key.h"
#include "runtime/boot.h"
#include "runtime/install.h"
#include "runtime/processes.h"
#include "runtime/updates.h"
#include "timestamp_text.h"
#include "tree_snapshot.h"

#define ENTRY_LOADER "entry-loader"
#define CLAUDE_LOADER "claude-loader"
#define BINARY "claude-binary"
#define UNREACHABLE_VERSION "99.0.0"
#define UNREACHABLE_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#define WRONG_RUN ((spisor_status)99)

/**
 * One install operation to fail allocation by allocation. `prepare` builds its base afresh with no failure, `run`
 * calls it, and `untouched`, when given, names what no answer may have done.
 */
typedef struct operation_case {
    const char *label;
    void (*prepare)(void);
    spisor_status (*run)(void);
    int (*untouched)(void);
} operation_case;

/** What a clean run answered and left, which an answer under failure other than OUT_OF_MEMORY must equal. */
typedef struct clean_run {
    spisor_status status;
    char refusal[600];
    char *snapshot;
    size_t snapshot_length;
    uint64_t allocations;
} clean_run;

static const runtime_install_definition *install;
static install_base base;
static spisor_value_tree application = SPISOR_INIT(spisor_value_tree);
static char *path_directory = NULL;
static char refusal_detail[600];
static spisor_value_tree updates_settings = SPISOR_INIT(spisor_value_tree);
static spisor_status last_status;
static const char *snapshot_left_out = NULL;

static void keep_refusal(spisor_refusal *refusal) {
    snprintf(refusal_detail, sizeof refusal_detail, "%s", refusal->detail == NULL ? "" : refusal->detail);
    install->release_refusal(install->context, refusal);
}

static void finish(void) {
    spisor_value_release(&application);
    spisor_value_release(&updates_settings);
    snapshot_left_out = NULL;
    free(path_directory);
    path_directory = NULL;
    runtime_install_use_runtime_build(NULL, NULL, NULL);
    finish_install_base(&base);
}

static char *place_binary(const char *directory) {
#if defined(_WIN32)
    write_in(directory, BINARY ".cmd", "@echo off\r\n");
#else
    char *path = joined(directory, BINARY);
    write_file(path, "#!/bin/sh\n");
    CHECK(chmod(path, 0755) == 0);
    free(path);
#endif
    return runtime_catalog_copy(directory);
}

static cJSON *loader_identity(const char *home) {
    cJSON *identity = cJSON_CreateObject();
    cJSON_AddStringToObject(identity, "id", ENTRY_LOADER);
    cJSON_AddStringToObject(identity, "version", "1.0.0");
    cJSON_AddStringToObject(identity, "home", home);
    cJSON_AddStringToObject(cJSON_AddObjectToObject(identity, "application"), "id", "sample");
    cJSON_AddFalseToObject(identity, "requiresCommand");
    cJSON_AddArrayToObject(identity, "commands");
    return identity;
}

static cJSON *catalog_provenance(void) {
    cJSON *provenance = cJSON_CreateObject();
    cJSON_AddStringToObject(provenance, "method", "catalog");
    cJSON_AddStringToObject(provenance, "version", "1.0.0");
    cJSON_AddStringToObject(provenance, "location", base.cache);
    cJSON_AddStringToObject(provenance, "os", "linux");
    cJSON_AddStringToObject(provenance, "architecture", "x64");
    cJSON_AddStringToObject(provenance, "installedAt", "2026-11-03T00:00:00.000Z");
    return provenance;
}

/** The entry loader registered with its home, as install registers it before writing its entry. */
static void register_entry_loader(void) {
    char *home = joined(base.user, "homes/sample");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_of(&base));
    cJSON_AddItemToObject(argument, "identity", loader_identity(home));
    cJSON_AddItemToObject(argument, "provenance", catalog_provenance());
    cJSON_AddItemToObject(argument, "environment", environment_of(&base));
    catalog_answer answer = hosts_call(HOSTS_REGISTER, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    free(home);
}

static void declare_hook_entry(void) {
    cJSON *declared = application_with_hook_entry("sample", "sample-binary", "{}");
    char *text = cJSON_PrintUnformatted(declared);
    CHECK(value_of_text(text, &application));
    cJSON_free(text);
    cJSON_Delete(declared);
}

/** A registered loader whose application declares the hook entry, a settings member among its changes. */
static void prepare_entry(void) {
    start_install_base(&base, "operations", "entry");
    char *homes = joined(base.user, "homes");
    CHECK(spisor_make_directories(homes) == 1);
    free(homes);
    register_entry_loader();
    declare_hook_entry();
}

/** As prepare_entry with the home's parent missing, so the disk refuses the home after the entry recorded changes. */
static void prepare_refused_entry(void) {
    start_install_base(&base, "operations", "entry-refused");
    register_entry_loader();
    declare_hook_entry();
}

static spisor_status run_entry(void) {
    runtime_install_entry_argument argument = SPISOR_INIT(runtime_install_entry_argument);
    runtime_install_entry_answer answer = SPISOR_INIT(runtime_install_entry_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char *home = joined(base.user, "homes/sample");
    argument.state = base.state;
    argument.cache = base.cache;
    argument.loader = ENTRY_LOADER;
    argument.home = home;
    argument.application = &application.root;
    spisor_status status = install->entry(install->context, &argument, &answer, &refusal);
    install->release_entry_answer(install->context, &answer);
    keep_refusal(&refusal);
    free(home);
    return status;
}

/** The catalog's claude loader installed through the ecosystem, claude found on PATH or installed through fake. */
static void install_claude(const char *packages_json, const char *choices_json) {
    list_foundation(&base);
    list_loader(&base, CLAUDE_LOADER, "1.0.0", application_json("claude", BINARY, packages_json), "[]", 0, "[]");
    write_catalog(&base);
    char *homes = joined(base.user, "homes/claude");
    CHECK(spisor_make_directories(homes) == 1);
    free(homes);
    char extra[256];
    snprintf(extra, sizeof extra, "{\"choices\": %s}", choices_json);
    cJSON_Delete(expect_report(install_with(&base, extra, environment_with_path(&base, path_directory)), "install"));
}

static void prepare_path_application(void) {
    start_install_base(&base, "operations", "path-application");
    char *directory = joined(base.root, "bin");
    path_directory = place_binary(directory);
    free(directory);
    install_claude("{}", "{}");
}

static void prepare_fake_application(void) {
    start_install_base(&base, "operations", "fake-application");
    path_directory = runtime_catalog_copy(base.path);
    install_claude("{\"fake\": \"claude-pkg\"}", "{\"claude\": \"fake\"}");
}

static spisor_status run_update(void) {
    runtime_install_application_argument argument = SPISOR_INIT(runtime_install_application_argument);
    runtime_install_application_answer answer = SPISOR_INIT(runtime_install_application_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.portable = base.portable;
    argument.state = base.state;
    argument.cache = base.cache;
    argument.loader = CLAUDE_LOADER;
    argument.operation = RUNTIME_INSTALL_UPDATE;
    spisor_status status = install->application(install->context, &argument, &answer, &refusal);
    install->release_application_answer(install->context, &answer);
    keep_refusal(&refusal);
    return status;
}

/** PATH as path_directory and the user's home under both names, so either platform's rule finds it. */
static void environment_of_path(runtime_variable environment[3]) {
    environment[0].name = "PATH";
    environment[0].value = path_directory;
    environment[1].name = "HOME";
    environment[1].value = base.user;
    environment[2].name = "USERPROFILE";
    environment[2].value = base.user;
}

static spisor_status run_register_loader(void) {
    runtime_install_register_loader_argument argument = SPISOR_INIT(runtime_install_register_loader_argument);
    runtime_install_register_loader_answer answer = SPISOR_INIT(runtime_install_register_loader_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_variable environment[3];
    environment_of_path(environment);
    argument.portable = base.portable;
    argument.state = base.state;
    argument.cache = base.cache;
    argument.environment = environment;
    argument.environment_count = 3;
    argument.id = CLAUDE_LOADER;
    argument.version = "1.0.0";
    spisor_status status = install->register_loader(install->context, &argument, &answer, &refusal);
    install->release_register_loader_answer(install->context, &answer);
    keep_refusal(&refusal);
    return status;
}

/** A node of major 22 on a PATH of its own, the pinned build pointed at an archive that is not there. */
static void prepare_runtime(void) {
    start_install_base(&base, "operations", "runtime");
    path_directory = joined(base.root, "path-node");
    write_in(path_directory, "version.txt", "22.1.0");
    size_t length = 0;
    uint8_t *probe = spisor_read_file(RUNTIME_NODE_PROBE_PATH, &length);
#if defined(_WIN32)
    char *node = joined(path_directory, "node.exe");
#else
    char *node = joined(path_directory, "node");
#endif
    CHECK(probe != NULL && spisor_replace_file(node, probe, length) == 1);
#if !defined(_WIN32)
    CHECK(chmod(node, 0755) == 0);
#endif
    free(node);
    free(probe);
    char *missing = joined(base.root, "unreachable/node.zip");
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, missing);
    runtime_install_use_runtime_build(UNREACHABLE_VERSION, url, UNREACHABLE_SHA256);
    free(url);
    free(missing);
}

static spisor_status run_runtime(void) {
    runtime_install_runtime_argument argument = SPISOR_INIT(runtime_install_runtime_argument);
    runtime_install_runtime_answer answer = SPISOR_INIT(runtime_install_runtime_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_variable environment[3];
    environment_of_path(environment);
    argument.cache = base.cache;
    argument.environment = environment;
    argument.environment_count = 3;
    spisor_status status = install->runtime(install->context, &argument, &answer, &refusal);
    install->release_runtime_answer(install->context, &answer);
    keep_refusal(&refusal);
    return status;
}

/** Whether nothing reached acquisition, which lays out the pinned runtime under cache/runtimes/node. */
static int acquired_nothing(void) {
    if (exists_in(base.cache, "runtimes/node")) {
        fprintf(stderr, "runtime: an answer reached acquisition with a node on the PATH\n");
        return 0;
    }
    return 1;
}

static void mark_version(const char *rest) {
    write_in(base.cache, rest, "{\"files\":{}}");
}

static void mark_library(const char *id, const char *version) {
    char rest[512];
    snprintf(rest, sizeof rest, "libraries/%s/%s/%s/.installed.json", id, version, runtime_platform_key());
    mark_version(rest);
}

static void settings_reading_the_catalog(void) {
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddItemToObject(cJSON_AddObjectToObject(settings, "catalog"), "sources", local_sources(&base));
    char *text = cJSON_PrintUnformatted(settings);
    CHECK(value_of_text(text, &updates_settings));
    cJSON_free(text);
    cJSON_Delete(settings);
}

/**
 * spisor-host registered, scheduled to switch to its installed 2.0.0, booting with two libraries of which the catalog
 * lists a newer one installed, so a start switches, records, hints and writes its run record.
 */
static void prepare_apply(void) {
    start_install_base(&base, "operations", "apply");
    list_library(&base, "runtime", "0.1.0");
    list_library(&base, "runtime", "0.2.0");
    list_library(&base, "koffi", "2.0.0");
    write_catalog(&base);
    mark_library("runtime", "0.1.0");
    mark_library("runtime", "0.2.0");
    mark_library("koffi", "2.0.0");
    mark_version("bundles/spisor-host/2.0.0/.installed.json");
    write_in(base.state, "hosts/spisor-host.json", "{\"id\":\"spisor-host\"}");
    write_in(base.state, "bundles/spisor-host.json",
             "{\"run\":{\"spisor-host\":\"1.0.0\"},\"next\":{\"spisor-host\":\"2.0.0\"}}");
    settings_reading_the_catalog();
    snapshot_left_out = "/state/bundles/runs";
}

static spisor_status run_apply(void) {
    static const runtime_item_version running[3] = {
        {"spisor-host", "1.0.0"}, {"runtime", "0.1.0"}, {"koffi", "2.0.0"}};
    static const char *const libraries[2] = {"runtime", "koffi"};
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_apply_argument argument = SPISOR_INIT(runtime_updates_apply_argument);
    runtime_updates_apply_answer answer = SPISOR_INIT(runtime_updates_apply_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    argument.settings = &updates_settings.root;
    argument.running = running;
    argument.running_count = 3;
    argument.writes_boot_hint = 1;
    argument.libraries = libraries;
    argument.library_count = 2;
    last_status = updates->apply(updates->context, &argument, &answer, &refusal);
    updates->release_apply_answer(updates->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Whether an apply that did not answer OK left no run record, as its header promises. */
static int recorded_no_run_unless_applied(void) {
    runtime_directory_listing listing = {NULL, 0};
    char *runs = joined(base.state, "bundles/runs/spisor-host");
    CHECK(runtime_list_directory(runs, &listing) == 1);
    free(runs);
    int kept = last_status == SPISOR_OK || listing.count == 0;
    runtime_free_directory_listing(&listing);
    if (!kept) {
        fprintf(stderr, "updates' apply: an answer of %d left a run record\n", (int)last_status);
    }
    return kept;
}

/** A host that switched from 1.0.0 to 2.0.0, both installed, with a boot hint naming 2.0.0. */
static void prepare_rollback(void) {
    start_install_base(&base, "operations", "rollback");
    mark_version("bundles/spisor-host/1.0.0/.installed.json");
    mark_version("bundles/spisor-host/2.0.0/.installed.json");
    write_in(base.cache, "hosts/spisor-host.json", "{\"libraries\":{},\"host\":\"2.0.0\"}");
    write_in(base.state, "hosts/spisor-host.json", "{\"id\":\"spisor-host\"}");
    write_in(base.state, "bundles/spisor-host.json",
             "{\"run\":{\"spisor-host\":\"2.0.0\"},\"next\":{},\"rollback\":{\"spisor-host\":\"1.0.0\"}}");
}

static spisor_status run_rollback(void) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_rollback_argument argument = SPISOR_INIT(runtime_updates_rollback_argument);
    runtime_updates_rollback_answer answer = SPISOR_INIT(runtime_updates_rollback_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    argument.item = "spisor-host";
    last_status = updates->rollback(updates->context, &argument, &answer, &refusal);
    updates->release_rollback_answer(updates->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Reads what the rollback base recorded running, answering WRONG_RUN for an OK that names anything else. */
static spisor_status run_recorded(void) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_recorded_argument argument = SPISOR_INIT(runtime_updates_recorded_argument);
    runtime_updates_recorded_answer answer = SPISOR_INIT(runtime_updates_recorded_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    last_status = updates->recorded(updates->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.run_count != 1 || strcmp(answer.run[0].version, "2.0.0") != 0)) {
        last_status = WRONG_RUN;
    }
    updates->release_recorded_answer(updates->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static const char *version_in(const char *place, const char *rest, const char *outer, const char *inner) {
    static char version[64];
    char *text = text_in(place, rest);
    cJSON *tree = text == NULL ? NULL : cJSON_Parse(text);
    const cJSON *map = inner == NULL ? tree : cJSON_GetObjectItemCaseSensitive(tree, outer);
    const char *found = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(map, inner == NULL ? outer : inner));
    snprintf(version, sizeof version, "%s", found == NULL ? "" : found);
    cJSON_Delete(tree);
    free(text);
    return version;
}

/** Whether an OK rollback rolled the references and the hint back, and any other answer left both as prepared. */
static int rolled_back_whole_or_not_at_all(void) {
    const char *expected = last_status == SPISOR_OK ? "1.0.0" : "2.0.0";
    int references = strcmp(version_in(base.state, "bundles/spisor-host.json", "run", "spisor-host"), expected) == 0;
    int hint = strcmp(version_in(base.cache, "hosts/spisor-host.json", "host", NULL), expected) == 0;
    if (!references || !hint) {
        fprintf(stderr, "updates' rollback: an answer of %d left the references %s and the hint %s\n",
                (int)last_status, references ? "as expected" : "otherwise", hint ? "as expected" : "otherwise");
    }
    return references && hint;
}

/** spisor-host running 1.0.0 while the local catalog lists 2.0.0, so a check fetches it, schedules it and sweeps. */
static void prepare_check(void) {
    start_install_base(&base, "operations", "check");
    list_host(&base, "spisor-host", "1.0.0", "native", NULL);
    list_host(&base, "spisor-host", "2.0.0", "native", NULL);
    write_catalog(&base);
    mark_version("bundles/spisor-host/1.0.0/.installed.json");
    write_in(base.state, "hosts/spisor-host.json", "{\"id\":\"spisor-host\"}");
    write_in(base.state, "bundles/spisor-host.json", "{\"run\":{\"spisor-host\":\"1.0.0\"},\"next\":{}}");
    settings_reading_the_catalog();
}

/** The check base with spisor-host registered at a home of its own, as a run reads every registration's settings. */
static void prepare_run(void) {
    prepare_check();
    char *home = joined(base.user, "homes/spisor-host");
    CHECK(spisor_make_directories(home) == 1);
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", "spisor-host");
    cJSON_AddStringToObject(registration, "home", home);
    char *text = cJSON_PrintUnformatted(registration);
    write_in(base.state, "hosts/spisor-host.json", text);
    cJSON_free(text);
    cJSON_Delete(registration);
    free(home);
}

static spisor_status run_run(void) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_run_argument argument = SPISOR_INIT(runtime_updates_run_argument);
    runtime_updates_run_answer answer = SPISOR_INIT(runtime_updates_run_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.portable = base.portable;
    argument.state = base.state;
    argument.cache = base.cache;
    argument.now = "2026-11-04T12:00:00.000Z";
    last_status = updates->run(updates->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.check_count != 1) {
        last_status = WRONG_RUN;
    }
    updates->release_run_answer(updates->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_check(void) {
    const runtime_updates_definition *updates = runtime_updates_definition_of();
    runtime_updates_check_argument argument = SPISOR_INIT(runtime_updates_check_argument);
    runtime_updates_check_answer answer = SPISOR_INIT(runtime_updates_check_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    argument.now = "2026-11-04T12:00:00.000Z";
    argument.settings = &updates_settings.root;
    last_status = updates->check(updates->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.fetched_count != 1 || answer.refused_count != 0)) {
        last_status = WRONG_RUN;
    }
    updates->release_check_answer(updates->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** The file key of `name` inside a host version's folder for this machine, with .exe on Windows. */
static void host_file_key(char *key, size_t capacity, const char *name) {
#if defined(_WIN32)
    snprintf(key, capacity, "%s/%s.exe", runtime_platform_key(), name);
#else
    snprintf(key, capacity, "%s/%s", runtime_platform_key(), name);
#endif
}

/**
 * spisor-host 1.0.0 installed with its executable and a spisor-boot recorded at their sizes, a library installed, and
 * a hint naming 1.0.0.
 */
static void prepare_boot(void) {
    char executable[64];
    char stub[64];
    char rest[512];
    char marker[1024];
    start_install_base(&base, "operations", "boot");
    host_file_key(executable, sizeof executable, "spisor-host");
    host_file_key(stub, sizeof stub, "spisor-boot");
    snprintf(rest, sizeof rest, "bundles/spisor-host/1.0.0/%s", executable);
    write_in(base.cache, rest, "x");
    snprintf(rest, sizeof rest, "bundles/spisor-host/1.0.0/%s", stub);
    write_in(base.cache, rest, "y");
    snprintf(marker, sizeof marker,
             "{\"files\":{\"%s\":\"%s\",\"%s\":\"%s\"},\"sizes\":{\"%s\":1,\"%s\":1}}", executable,
             UNREACHABLE_SHA256, stub, UNREACHABLE_SHA256, executable, stub);
    write_in(base.cache, "bundles/spisor-host/1.0.0/.installed.json", marker);
    mark_library("runtime", "0.1.0");
    write_in(base.cache, "hosts/spisor-host.json", "{\"libraries\":{},\"host\":\"1.0.0\"}");
}

static spisor_status run_boot_read(void) {
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    runtime_boot_host_argument argument = SPISOR_INIT(runtime_boot_host_argument);
    runtime_boot_hint_answer answer = SPISOR_INIT(runtime_boot_hint_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    last_status = boot->read(boot->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (!answer.hinted || answer.host == NULL || strcmp(answer.host, "1.0.0") != 0)) {
        last_status = WRONG_RUN;
    }
    boot->release_hint_answer(boot->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_boot_write(void) {
    static const runtime_item_version libraries[1] = {{"runtime", "0.1.0"}};
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    runtime_boot_write_argument argument = SPISOR_INIT(runtime_boot_write_argument);
    runtime_boot_hint_answer answer = SPISOR_INIT(runtime_boot_hint_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    argument.libraries = libraries;
    argument.library_count = 1;
    argument.state = base.state;
    last_status = boot->write(boot->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.library_count != 1 || answer.host == NULL)) {
        last_status = WRONG_RUN;
    }
    boot->release_hint_answer(boot->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_boot_resolve(void) {
    static const char *const arguments[1] = {"--version"};
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    runtime_boot_resolve_argument argument = SPISOR_INIT(runtime_boot_resolve_argument);
    runtime_boot_resolve_answer answer = SPISOR_INIT(runtime_boot_resolve_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.cache = base.cache;
    argument.host_id = "spisor-host";
    argument.arguments = arguments;
    argument.argument_count = 1;
    last_status = boot->resolve(boot->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.argument_count != 1 || answer.executable == NULL)) {
        last_status = WRONG_RUN;
    }
    boot->release_resolve_answer(boot->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_boot_place(void) {
    const runtime_boot_definition *boot = runtime_boot_definition_of();
    runtime_boot_place_argument argument = SPISOR_INIT(runtime_boot_place_argument);
    runtime_boot_place_answer answer = SPISOR_INIT(runtime_boot_place_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.cache = base.cache;
    last_status = boot->place(boot->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && !answer.placed) {
        last_status = WRONG_RUN;
    }
    boot->release_place_answer(boot->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Whether a place that did not answer OK left no spisor-boot in cache/boot, as its header promises. */
static int placed_nothing_unless_placed(void) {
    if (last_status != SPISOR_OK && exists_in(base.cache, "boot")) {
        runtime_directory_listing listing = {NULL, 0};
        char *folder = joined(base.cache, "boot");
        CHECK(runtime_list_directory(folder, &listing) == 1);
        free(folder);
        int empty = listing.count == 0;
        runtime_free_directory_listing(&listing);
        if (!empty) {
            fprintf(stderr, "boot's place: an answer of %d left spisor-boot in place\n", (int)last_status);
        }
        return empty;
    }
    return 1;
}

#define BACKGROUND_HOST "background-host"
#define GONE_PID 2147480000.0
#define STAMPED_AT 1800000000000.0

static char prepared_holders[512];
static char live_record[1024];
static double live_stamp;

/** The holders file of BACKGROUND_HOST naming this process as the host and as a holder, and one holder long gone. */
static void prepare_processes(void) {
    start_install_base(&base, "operations", "processes");
    live_stamp = runtime_now_milliseconds();
    snprintf(prepared_holders, sizeof prepared_holders,
             "{\"pid\":%d,\"aliveAt\":%.0f,\"checkMilliseconds\":600000,\"holders\":[%d,%.0f]}\n",
             (int)fixture_process_id(), live_stamp, (int)fixture_process_id(), GONE_PID);
    write_in(base.state, "processes/" BACKGROUND_HOST ".json", prepared_holders);
    snprintf(live_record, sizeof live_record, "bundles/runs/%s/%d.1-1.json", BACKGROUND_HOST,
             (int)fixture_process_id());
    char text[512];
    snprintf(text, sizeof text,
             "{\"pid\":%d,\"aliveAt\":%.0f,\"checkMilliseconds\":600000,\"run\":{\"a\":\"1.0.0\"}}\n",
             (int)fixture_process_id(), STAMPED_AT);
    write_in(base.state, live_record, text);
    snprintf(text, sizeof text,
             "{\"pid\":%.0f,\"aliveAt\":%.0f,\"checkMilliseconds\":5000,\"run\":{\"a\":\"0.9.0\"}}\n", GONE_PID,
             STAMPED_AT);
    write_in(base.state, "bundles/runs/" BACKGROUND_HOST "/gone.json", text);
    snprintf(text, sizeof text, "{\"pid\":%d,\"environment\":{\"PORT\":\"1\"}}\n", (int)fixture_process_id());
    write_in(base.state, "processes/" BACKGROUND_HOST ".ready.json", text);
    snapshot_left_out = "/state/processes";
}

/** As prepare_processes, leaving out the state, whose holders and new run records carry the time. */
static void prepare_record(void) {
    prepare_processes();
    snapshot_left_out = "/state";
}

/** Whether a call that did not answer OK left the holders file as prepared. */
static int holders_unchanged_unless_ok(void) {
    char *text = text_in(base.state, "processes/" BACKGROUND_HOST ".json");
    int kept = last_status == SPISOR_OK || (text != NULL && strcmp(text, prepared_holders) == 0);
    free(text);
    if (!kept) {
        fprintf(stderr, "processes: an answer of %d changed the holders file\n", (int)last_status);
    }
    return kept;
}

/** Whether the run record of this live process is still there, whatever was answered. */
static int kept_the_live_record(void) {
    if (!exists_in(base.state, live_record)) {
        fprintf(stderr, "processes: an answer of %d deleted a live run record\n", (int)last_status);
        return 0;
    }
    return 1;
}

static const runtime_processes_definition *processes_service(void) {
    return runtime_processes_definition_of();
}

static spisor_status run_hold(void) {
    static const char *const arguments[1] = {"--background"};
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_hold_argument argument = SPISOR_INIT(runtime_processes_hold_argument);
    runtime_processes_hold_answer answer = SPISOR_INIT(runtime_processes_hold_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.holder = (int64_t)fixture_process_id();
    argument.executable = base.cache;
    argument.arguments = arguments;
    argument.argument_count = 1;
    last_status = processes->hold(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.started || answer.pid != (int64_t)fixture_process_id())) {
        last_status = WRONG_RUN;
    }
    processes->release_hold_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_release(void) {
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_holder_argument argument = SPISOR_INIT(runtime_processes_holder_argument);
    runtime_processes_release_answer answer = SPISOR_INIT(runtime_processes_release_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.holder = (int64_t)fixture_process_id();
    last_status = processes->release(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.holder_count != 0) {
        last_status = WRONG_RUN;
    }
    processes->release_release_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_prune(void) {
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_prune_argument argument = SPISOR_INIT(runtime_processes_prune_argument);
    runtime_processes_prune_answer answer = SPISOR_INIT(runtime_processes_prune_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.self = (int64_t)fixture_process_id();
    argument.now = live_stamp + 1;
    last_status = processes->prune(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.exit) {
        last_status = WRONG_RUN;
    }
    processes->release_prune_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_ready(void) {
    static const runtime_variable environment[1] = {{"PORT", "2"}};
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_ready_argument argument = SPISOR_INIT(runtime_processes_ready_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.pid = (int64_t)fixture_process_id();
    argument.environment = environment;
    argument.environment_count = 1;
    last_status = processes->ready(processes->context, &argument, &refusal);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_await_ready(void) {
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_await_argument argument = SPISOR_INIT(runtime_processes_await_argument);
    runtime_processes_await_answer answer = SPISOR_INIT(runtime_processes_await_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.pid = (int64_t)fixture_process_id();
    argument.timeout_milliseconds = 200;
    last_status = processes->await_ready(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.environment_count != 1) {
        last_status = WRONG_RUN;
    }
    processes->release_await_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_record_run(void) {
    static const runtime_item_version run[1] = {{"a", "2.0.0"}};
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_record_argument argument = SPISOR_INIT(runtime_processes_record_argument);
    runtime_processes_record_answer answer = SPISOR_INIT(runtime_processes_record_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.run = run;
    argument.run_count = 1;
    last_status = processes->record_run(processes->context, &argument, &answer, &refusal);
    processes->release_record_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Whether a record_run that did not answer OK left only the two prepared run records. */
static int recorded_no_new_run_unless_ok(void) {
    runtime_directory_listing listing = {NULL, 0};
    char *runs = joined(base.state, "bundles/runs/" BACKGROUND_HOST);
    CHECK(runtime_list_directory(runs, &listing) == 1);
    free(runs);
    int kept = last_status == SPISOR_OK || listing.count == 2;
    runtime_free_directory_listing(&listing);
    if (!kept) {
        fprintf(stderr, "processes' record_run: an answer of %d left a new run record\n", (int)last_status);
    }
    return kept;
}

static spisor_status run_live_runs(void) {
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_live_argument argument = SPISOR_INIT(runtime_processes_live_argument);
    runtime_processes_live_answer answer = SPISOR_INIT(runtime_processes_live_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.now = STAMPED_AT + 1;
    last_status = processes->live_runs(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.run_count != 1) {
        last_status = WRONG_RUN;
    }
    processes->release_live_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static spisor_status run_remove_runs(void) {
    const runtime_processes_definition *processes = processes_service();
    runtime_processes_remove_argument argument = SPISOR_INIT(runtime_processes_remove_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char *gone = joined(base.state, "bundles/runs/" BACKGROUND_HOST "/gone.json");
    argument.state = base.state;
    argument.host_id = BACKGROUND_HOST;
    argument.record = gone;
    last_status = processes->remove_runs(processes->context, &argument, &refusal);
    free(gone);
    keep_refusal(&refusal);
    return last_status;
}

/** Whether a remove_runs that did not answer OK left the record it was asked to delete. */
static int removed_nothing_unless_ok(void) {
    int kept = last_status == SPISOR_OK || exists_in(base.state, "bundles/runs/" BACKGROUND_HOST "/gone.json");
    if (!kept) {
        fprintf(stderr, "processes' remove_runs: an answer of %d deleted the record\n", (int)last_status);
    }
    return kept && kept_the_live_record();
}

#define STARTED_HOST "started-host"
/** How long a host a failed hold might still have started is given to write its marker, which no host may write. */
#define UNSTARTED_WAIT_MILLISECONDS 1000

static char start_marker[1024];
static unsigned start_marker_number;
static int64_t started_pid;
static int started_host_ran;

/** A state holding nothing for STARTED_HOST, and a marker path no earlier run used, which the started host writes. */
static void prepare_hold_start(void) {
    remove(start_marker);
    start_install_base(&base, "operations", "hold-start");
    snprintf(start_marker, sizeof start_marker, "%s/hold-start-marker-%d-%u.json", RUNTIME_SCRATCH_PATH,
             (int)fixture_process_id(), ++start_marker_number);
    started_pid = 0;
    started_host_ran = 0;
    snapshot_left_out = "/state";
}

static int marker_written(void) {
    for (int waited = 0; waited < 10000 && !spisor_file_exists(start_marker); waited += 50) {
        pause_milliseconds(50);
    }
    return spisor_file_exists(start_marker);
}

static spisor_status run_hold_start(void) {
    const char *const arguments[3] = {"environment", start_marker, "SPISOR_FIXTURE_VALUE"};
    const runtime_processes_definition *processes = runtime_processes_definition_of();
    runtime_processes_hold_argument argument = SPISOR_INIT(runtime_processes_hold_argument);
    runtime_processes_hold_answer answer = SPISOR_INIT(runtime_processes_hold_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.host_id = STARTED_HOST;
    argument.holder = (int64_t)fixture_process_id();
    argument.executable = RUNTIME_BACKGROUND_FIXTURE_PATH;
    argument.arguments = arguments;
    argument.argument_count = 3;
    argument.directory = RUNTIME_SCRATCH_PATH;
    last_status = processes->hold(processes->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK) {
        started_pid = answer.pid;
        started_host_ran = marker_written();
        kill_pid((long)started_pid);
        last_status = answer.started ? SPISOR_OK : WRONG_RUN;
    }
    processes->release_hold_answer(processes->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static int holders_name_started_host(void) {
    char *text = text_in(base.state, "processes/" STARTED_HOST ".json");
    char host[64];
    snprintf(host, sizeof host, "\"pid\":%lld,", (long long)started_pid);
    int named = text != NULL && strstr(text, host) != NULL;
    free(text);
    return named;
}

/** Whether an OK hold recorded the host it started, and any other answer started no host and recorded no holder. */
static int started_and_recorded_or_nothing(void) {
    int kept;
    if (last_status == SPISOR_OK) {
        kept = holders_name_started_host() && started_host_ran;
    } else {
        pause_milliseconds(UNSTARTED_WAIT_MILLISECONDS);
        kept = !spisor_file_exists(start_marker) && !exists_in(base.state, "processes/" STARTED_HOST ".json");
    }
    if (!kept) {
        fprintf(stderr, "processes' hold: an answer of %d %s\n", (int)last_status,
                last_status == SPISOR_OK ? "did not record the host it started" : "started a host or recorded holders");
    }
    return kept;
}

#define OOM_HOST "oom-host"
#define PROVENANCE_NOW 1762128000000.0

static char register_home[1024];
static char register_target[1024];

static const runtime_hosts_definition *hosts_service(void) {
    return runtime_hosts_definition_of();
}

/** A binary on a PATH of its own for a command's shim to run, and no user Path entry an earlier attempt left. */
static void prepare_register(void) {
#if defined(_WIN32)
    delete_test_path_value();
#endif
    start_install_base(&base, "operations", "hosts-register");
    path_directory = place_binary(base.path);
    snprintf(register_home, sizeof register_home, "%s/homes/oom", base.user);
#if defined(_WIN32)
    snprintf(register_target, sizeof register_target, "%s/" BINARY ".cmd", path_directory);
#else
    snprintf(register_target, sizeof register_target, "%s/" BINARY, path_directory);
#endif
}

static spisor_status run_register(void) {
    static const char *const command_arguments[1] = {"--serve"};
    const runtime_hosts_definition *hosts = hosts_service();
    runtime_registry_provenance provenance = SPISOR_INIT(runtime_registry_provenance);
    runtime_registration registration = SPISOR_INIT(runtime_registration);
    runtime_hosts_command command = {"oomhost", register_target, command_arguments, 1};
    runtime_hosts_register_argument argument = SPISOR_INIT(runtime_hosts_register_argument);
    runtime_install_register_loader_answer answer = SPISOR_INIT(runtime_install_register_loader_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_variable environment[3];
    environment_of_path(environment);
    provenance.method = "catalog";
    provenance.version = "1.0.0";
    provenance.location = base.cache;
    provenance.os = "linux";
    provenance.architecture = "x64";
    provenance.installed_at = "2026-11-03T00:00:00.000Z";
    registration.id = OOM_HOST;
    registration.version = "1.0.0";
    registration.home = register_home;
    registration.provenance = &provenance;
    argument.state = base.state;
    argument.registration = &registration;
    argument.commands = &command;
    argument.command_count = 1;
    argument.environment = environment;
    argument.environment_count = 3;
    last_status = hosts->register_host(hosts->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.command_count != 1 || answer.provenance == NULL)) {
        last_status = WRONG_RUN;
    }
    hosts->release_register_answer(hosts->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

static void prepare_list(void) {
    start_install_base(&base, "operations", "hosts-list");
    register_entry_loader();
}

static spisor_status run_list(void) {
    const runtime_hosts_definition *hosts = hosts_service();
    runtime_registry_list_argument argument = SPISOR_INIT(runtime_registry_list_argument);
    runtime_registry_listing answer = SPISOR_INIT(runtime_registry_listing);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    last_status = hosts->list(hosts->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && answer.host_count != 1) {
        last_status = WRONG_RUN;
    }
    hosts->release_listing(hosts->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** The entry loader registered with its entry written, so a removal has recorded changes to reverse. */
static void prepare_remove(void) {
    prepare_entry();
    CHECK(run_entry() == SPISOR_OK);
}

static spisor_status run_remove(void) {
    const runtime_hosts_definition *hosts = hosts_service();
    runtime_registry_forget_argument argument = SPISOR_INIT(runtime_registry_forget_argument);
    runtime_hosts_remove_answer answer = SPISOR_INIT(runtime_hosts_remove_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.state = base.state;
    argument.cache = base.cache;
    argument.id = ENTRY_LOADER;
    last_status = hosts->remove(hosts->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.reversed == 0 || !answer.forgotten)) {
        last_status = WRONG_RUN;
    }
    hosts->release_remove_answer(hosts->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Whether a removal that did not answer OK kept the registration, and with it what is left to undo. */
static int kept_the_registration_unless_removed(void) {
    int kept = last_status == SPISOR_OK || exists_in(base.state, "hosts/" ENTRY_LOADER ".json");
    if (!kept) {
        fprintf(stderr, "hosts' remove: an answer of %d forgot the registration\n", (int)last_status);
    }
    return kept;
}

static void prepare_provenance(void) {
    start_install_base(&base, "operations", "hosts-provenance");
    path_directory = place_binary(base.path);
}

static spisor_status run_provenance(void) {
    const runtime_hosts_definition *hosts = hosts_service();
    runtime_install_origin origin = {{"npm", "oom-package", NULL}, "1.0.0"};
    runtime_hosts_provenance_argument argument = SPISOR_INIT(runtime_hosts_provenance_argument);
    runtime_hosts_provenance_answer answer = SPISOR_INIT(runtime_hosts_provenance_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_variable environment[3];
    environment_of_path(environment);
    argument.origin = &origin;
    argument.binary = BINARY;
    argument.home = base.user;
    argument.environment = environment;
    argument.environment_count = 3;
    argument.now = PROVENANCE_NOW;
    last_status = hosts->provenance(hosts->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.provenance == NULL || answer.provenance->location == NULL)) {
        last_status = WRONG_RUN;
    }
    hosts->release_provenance_answer(hosts->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** updates' apply world, spisor-host registered with a version and a home, so a start plans and records a run. */
static void prepare_start(void) {
    cJSON *registration = cJSON_CreateObject();
    char *home;
    char *text;
    prepare_apply();
    home = joined(base.user, "application");
    cJSON_AddStringToObject(registration, "id", "spisor-host");
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", "1.0.0");
    cJSON_AddStringToObject(registration, "home", home);
    text = cJSON_PrintUnformatted(registration);
    write_in(base.state, "hosts/spisor-host.json", text);
    cJSON_free(text);
    cJSON_Delete(registration);
    free(home);
}

static spisor_status run_start(void) {
    static const runtime_item_version layer = {"koffi", "2.0.0"};
    static const runtime_item_version libraries[1] = {{"runtime", "0.1.0"}};
    const runtime_hosts_definition *hosts = hosts_service();
    runtime_hosts_start_argument argument = SPISOR_INIT(runtime_hosts_start_argument);
    runtime_hosts_start_answer answer = SPISOR_INIT(runtime_hosts_start_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    runtime_variable environment[5] = {{"HOME", base.user},
                                       {"USERPROFILE", base.user},
                                       {"SPISOR_HOME", base.portable},
                                       {"SPISOR_STATE", base.state},
                                       {"SPISOR_CACHE", base.cache}};
    argument.environment = environment;
    argument.environment_count = 5;
    argument.host_id = "spisor-host";
    argument.layer = &layer;
    argument.libraries = libraries;
    argument.library_count = 1;
    argument.session = 1;
    last_status = hosts->start(hosts->context, &argument, &answer, &refusal);
    if (last_status == SPISOR_OK && (answer.plan == NULL || answer.plan->run_record == NULL || answer.timers == NULL)) {
        last_status = WRONG_RUN;
    }
    hosts->release_start_answer(hosts->context, &answer);
    keep_refusal(&refusal);
    return last_status;
}

/** Everything under the base, its timestamps replaced; freed with free. */
static char *base_snapshot(size_t *length) {
    size_t raw_length = 0;
    char *raw = tree_snapshot_sized(base.root, snapshot_left_out, &raw_length);
    char *normalized = without_timestamps((const uint8_t *)raw, raw_length, length);
    free(raw);
    return normalized;
}

static void run_clean(const operation_case *each, clean_run *clean) {
    each->prepare();
    spisor_memory_fail_at(0);
    clean->status = each->run();
    clean->allocations = spisor_memory_attempts();
    snprintf(clean->refusal, sizeof clean->refusal, "%s", refusal_detail);
    clean->snapshot = base_snapshot(&clean->snapshot_length);
    finish();
}

/** Whether an answer other than OUT_OF_MEMORY is the clean run's, its refusal and every byte it left alike. */
static int matches_clean(const clean_run *clean, spisor_status status, const char *label, uint64_t attempt) {
    size_t length = 0;
    char *snapshot = base_snapshot(&length);
    int same_tree = snapshot != NULL && length == clean->snapshot_length &&
                    memcmp(snapshot, clean->snapshot, length) == 0;
    free(snapshot);
    if (status == clean->status && strcmp(refusal_detail, clean->refusal) == 0 && same_tree) {
        return 1;
    }
    fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"%s; a clean run answers %d \"%s\"\n", label,
            (unsigned long long)attempt, (int)status, refusal_detail, same_tree ? "" : " leaving another tree",
            (int)clean->status, clean->refusal);
    return 0;
}

static int sticky_failures = 0;
static int every_failure_answered = 0;

/** Fails allocation `attempt`, and with sticky failures every one after it as well. */
static void fail_allocation(uint64_t attempt) {
    if (sticky_failures && attempt != 0) {
        spisor_memory_fail_from(attempt);
    } else {
        spisor_memory_fail_at(attempt);
    }
}

/** Fails allocation attempt; 1 to go on, 0 after a failure the case does not allow, and *ran_whole when none failed. */
static int fail_once(const operation_case *each, const clean_run *clean, uint64_t attempt, int *ran_whole) {
    each->prepare();
    int64_t baseline = spisor_memory_live_blocks();
    refusal_detail[0] = 0;
    fail_allocation(attempt);
    spisor_status status = each->run();
    *ran_whole = !spisor_memory_failed();
    spisor_memory_fail_at(0);
    int kept = spisor_memory_live_blocks() == baseline;
    if (!kept) {
        fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", each->label,
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
    }
    kept = kept && (each->untouched == NULL || each->untouched());
    if (kept && every_failure_answered && !*ran_whole && status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "%s: failing allocation %llu answered %d \"%s\"\n", each->label, (unsigned long long)attempt,
                (int)status, refusal_detail);
        kept = 0;
    }
    if (kept && (status != SPISOR_OUT_OF_MEMORY || *ran_whole)) {
        kept = matches_clean(clean, status, each->label, attempt);
    }
    finish();
    return kept;
}

/** Fails the operation's allocations one at a time, between first and last when given, until it runs whole. */
static void every_failure_of(const operation_case *each, spisor_status expected, uint64_t first, uint64_t last) {
    clean_run clean;
    run_clean(each, &clean);
    if (clean.status != expected) {
        fprintf(stderr, "%s: a clean run answered %d \"%s\"\n", each->label, (int)clean.status, clean.refusal);
        check_failures++;
    }
    uint64_t attempt = first;
    int ran_whole = 0;
    for (; check_failures == 0 && !ran_whole && (last == 0 || attempt <= last); attempt++) {
        if (!fail_once(each, &clean, attempt, &ran_whole)) {
            check_failures++;
        }
    }
    printf("%s: %llu allocations, failures %llu to %llu checked\n", each->label,
           (unsigned long long)clean.allocations, (unsigned long long)first, (unsigned long long)(attempt - 1));
    free(clean.snapshot);
}

static int is_selected(const char *only, const char *label) {
    return strncmp(label, only, strlen(only)) == 0;
}

/** With a label's start as the first argument runs only the cases it starts, failing allocations first to last. */
int main(int argc, char **argv) {
    static const operation_case entry = {"entry", prepare_entry, run_entry, NULL};
    static const operation_case refused_entry = {"entry refused", prepare_refused_entry, run_entry, NULL};
    static const operation_case update = {"application", prepare_fake_application, run_update, NULL};
    static const operation_case path_update = {"application on path", prepare_path_application, run_update, NULL};
    static const operation_case found = {"runtime", prepare_runtime, run_runtime, acquired_nothing};
    static const operation_case registered = {"register_loader", prepare_path_application, run_register_loader, NULL};
    static const operation_case applied = {"updates' apply", prepare_apply, run_apply, recorded_no_run_unless_applied};
    static const operation_case rolled = {"updates' rollback", prepare_rollback, run_rollback,
                                          rolled_back_whole_or_not_at_all};
    static const operation_case ran = {"updates' run", prepare_run, run_run, NULL};
    static const operation_case read = {"updates' recorded", prepare_rollback, run_recorded, NULL};
    static const operation_case checked = {"updates' check", prepare_check, run_check, NULL};
    static const operation_case boot_cases[4] = {{"boot's read", prepare_boot, run_boot_read, NULL},
                                                 {"boot's write", prepare_boot, run_boot_write, NULL},
                                                 {"boot's resolve", prepare_boot, run_boot_resolve, NULL},
                                                 {"boot's place", prepare_boot, run_boot_place,
                                                  placed_nothing_unless_placed}};
    static const operation_case processes_cases[8] = {
        {"processes' hold", prepare_processes, run_hold, holders_unchanged_unless_ok},
        {"processes' release", prepare_processes, run_release, NULL},
        {"processes' prune", prepare_processes, run_prune, holders_unchanged_unless_ok},
        {"processes' ready", prepare_processes, run_ready, NULL},
        {"processes' await_ready", prepare_processes, run_await_ready, NULL},
        {"processes' record_run", prepare_record, run_record_run, recorded_no_new_run_unless_ok},
        {"processes' live_runs", prepare_processes, run_live_runs, kept_the_live_record},
        {"processes' remove_runs", prepare_processes, run_remove_runs, removed_nothing_unless_ok}};
    static const operation_case hosts_cases[5] = {
        {"hosts' register_host", prepare_register, run_register, NULL},
        {"hosts' list", prepare_list, run_list, NULL},
        {"hosts' remove", prepare_remove, run_remove, kept_the_registration_unless_removed},
        {"hosts' provenance", prepare_provenance, run_provenance, NULL},
        {"hosts' start", prepare_start, run_start, recorded_no_run_unless_applied}};
    static const operation_case hold_start = {"processes' hold starting", prepare_hold_start, run_hold_start,
                                              started_and_recorded_or_nothing};
    const char *only = argc > 1 ? argv[1] : "";
    uint64_t first = argc > 2 ? strtoull(argv[2], NULL, 10) : 1;
    uint64_t last = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
    spisor_json_use_library_allocator();
    install = runtime_install_definition_of();
    if (is_selected(only, entry.label) && strcmp(only, refused_entry.label) != 0) {
        every_failure_of(&entry, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, refused_entry.label)) {
        every_failure_of(&refused_entry, SPISOR_REFUSED, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, update.label) && strcmp(only, path_update.label) != 0) {
        every_failure_of(&update, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, path_update.label)) {
        every_failure_of(&path_update, SPISOR_REFUSED, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, found.label)) {
        every_failure_of(&found, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, registered.label)) {
        every_failure_of(&registered, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, applied.label)) {
        every_failure_of(&applied, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, rolled.label)) {
        every_failure_answered = 1;
        every_failure_of(&rolled, SPISOR_OK, first == 0 ? 1 : first, last);
        sticky_failures = 1;
        every_failure_of(&rolled, SPISOR_OK, first == 0 ? 1 : first, last);
        sticky_failures = 0;
        every_failure_answered = 0;
    }
    if (is_selected(only, read.label)) {
        every_failure_of(&read, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    for (size_t index = 0; index < 2; index++) {
        const operation_case *each = index == 0 ? &checked : &ran;
        if (is_selected(only, each->label)) {
            every_failure_answered = 1;
            every_failure_of(each, SPISOR_OK, first == 0 ? 1 : first, last);
            sticky_failures = 1;
            every_failure_of(each, SPISOR_OK, first == 0 ? 1 : first, last);
            sticky_failures = 0;
            every_failure_answered = 0;
        }
    }
    for (size_t index = 0; index < 4; index++) {
        if (is_selected(only, boot_cases[index].label)) {
            every_failure_of(&boot_cases[index], SPISOR_OK, first == 0 ? 1 : first, last);
        }
    }
    for (size_t index = 0; index < 8; index++) {
        if (is_selected(only, processes_cases[index].label)) {
            every_failure_of(&processes_cases[index], SPISOR_OK, first == 0 ? 1 : first, last);
        }
    }
    if (is_selected(only, hold_start.label)) {
        every_failure_of(&hold_start, SPISOR_OK, first == 0 ? 1 : first, last);
        sticky_failures = 1;
        every_failure_of(&hold_start, SPISOR_OK, first == 0 ? 1 : first, last);
        sticky_failures = 0;
    }
    for (size_t index = 0; index < 5; index++) {
        if (is_selected(only, hosts_cases[index].label)) {
            every_failure_of(&hosts_cases[index], SPISOR_OK, first == 0 ? 1 : first, last);
            sticky_failures = 1;
            every_failure_of(&hosts_cases[index], SPISOR_OK, first == 0 ? 1 : first, last);
            sticky_failures = 0;
        }
    }
    return check_summary("out_of_memory_install_operations_test");
}
