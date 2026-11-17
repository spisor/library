#include "install_fixture.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "allocation.h"
#include "catalog_text.h"
#include "definition_fixture.h"
#include "directory_tree.h"
#include "hosts.h"
#include "install_hooks.h"
#include "json_allocation.h"
#include "platform_key.h"
#include "runtime/install.h"
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
    catalog_answer answer = call_catalog(runtime_hosts_register, argument);
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
    static const runtime_updates_version running[3] = {
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

/** Fails allocation attempt; 1 to go on, 0 after a failure the case does not allow, and *ran_whole when none failed. */
static int fail_once(const operation_case *each, const clean_run *clean, uint64_t attempt, int *ran_whole) {
    each->prepare();
    int64_t baseline = spisor_memory_live_blocks();
    refusal_detail[0] = 0;
    spisor_memory_fail_at(attempt);
    spisor_status status = each->run();
    *ran_whole = !spisor_memory_failed();
    spisor_memory_fail_at(0);
    int kept = spisor_memory_live_blocks() == baseline;
    if (!kept) {
        fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", each->label,
                (unsigned long long)attempt, (long long)(spisor_memory_live_blocks() - baseline));
    }
    kept = kept && (each->untouched == NULL || each->untouched());
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
    static const operation_case rolled = {"updates' rollback", prepare_rollback, run_rollback, NULL};
    static const operation_case read = {"updates' recorded", prepare_rollback, run_recorded, NULL};
    static const operation_case checked = {"updates' check", prepare_check, run_check, NULL};
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
        every_failure_of(&rolled, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, read.label)) {
        every_failure_of(&read, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    if (is_selected(only, checked.label)) {
        every_failure_of(&checked, SPISOR_OK, first == 0 ? 1 : first, last);
    }
    return check_summary("out_of_memory_install_operations_test");
}
