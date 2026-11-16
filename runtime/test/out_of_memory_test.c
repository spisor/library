#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "allocation.h"
#include "check.h"
#include "definition_fixture.h"
#include "json_allocation.h"
#include "runtime/changes.h"
#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/log.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/registry.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
#include "scratch_directory.h"
#include "registry.h"
#include "spisor/file.h"

/**
 * @file
 * Fails each allocation of one operation per service in turn, cJSON's among them, and requires every failure to answer
 * OUT_OF_MEMORY and to leave no block of the binary's allocator live.
 */

#if defined(_WIN32)
#define LOCAL_APP_DATA {"LOCALAPPDATA", "C:\\L"}
#else
#define LOCAL_APP_DATA {"UNUSED", ""}
#endif

#define START "2026-10-27T19:00:00.000Z"
#define CHANGED_FILE "{\"a\":{\"b\":1},\"logging\":true}"
#define TIME "2026-10-27T19:34:53.123Z"

typedef struct scratch {
    char base[1024];
    char home[1100];
    char portable[1100];
    char secrets_file[1200];
    char config_directory[1200];
    char programs[1200];
    char state[1200];
} scratch;

static scratch place;
static spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree config = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree first_file = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree second_file = SPISOR_INIT(spisor_value_tree);

static void write_text(const char *directory, const char *name, const char *text) {
    char path[1400];
    CHECK(spisor_make_directories(directory) == 1);
    snprintf(path, sizeof path, "%s/%s", directory, name);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

/** The files every operation reads, written afresh before each attempt so no attempt sees another's writes. */
static void fresh_files(void) {
    start_empty_directory(place.base);
    write_text(place.config_directory, "p.json", CHANGED_FILE);
    write_text(place.config_directory, "settings.json", "{\"logConsole\":true,\"ui\":{\"size\":2}}");
    write_text(place.portable, "settings.json", "{\"ui\":{\"theme\":\"dark\"}}");
    write_text(place.programs, "probe", "");
    char record[2400];
    char hosts[1300];
    snprintf(hosts, sizeof hosts, "%s/hosts", place.state);
    snprintf(record, sizeof record,
             "{\"id\":\"changer\",\"kind\":\"host\",\"version\":\"1\",\"home\":\"%s\",\"provenance\":"
             "{\"method\":\"npm\",\"version\":\"1\",\"location\":\"%s\",\"os\":\"linux\",\"architecture\":\"x64\","
             "\"installedAt\":\"2026-11-02T10:00:00Z\"},\"requiresCommand\":false,\"commands\":[],\"changes\":[]}",
             place.home, place.home);
    write_text(hosts, "changer.json", record);
    snprintf(record, sizeof record,
             "{\"id\":\"settler\",\"kind\":\"host\",\"version\":\"1\",\"home\":\"%s\",\"provenance\":"
             "{\"method\":\"npm\",\"version\":\"1\",\"location\":\"%s\",\"os\":\"linux\",\"architecture\":\"x64\","
             "\"installedAt\":\"2026-11-02T10:00:00Z\"},\"requiresCommand\":false,\"commands\":[],\"changes\":["
             "{\"sequence\":1,\"kind\":\"json-key\",\"state\":\"pending\",\"reversed\":false,"
             "\"file\":\"%s/p.json\",\"key\":[\"a\",\"b\"],\"value\":1}],\"nextSequence\":2}",
             place.home, place.home, place.config_directory);
    write_text(hosts, "settler.json", record);
#ifndef _WIN32
    char probe[1300];
    snprintf(probe, sizeof probe, "%s/probe", place.programs);
    CHECK(chmod(probe, 0755) == 0);
#endif
}

typedef spisor_status (*operation)(void);

static spisor_status resolve_paths(void) {
    const runtime_paths_definition *paths = runtime_paths_definition_of();
    runtime_paths_resolve_argument argument = SPISOR_INIT(runtime_paths_resolve_argument);
    runtime_paths_answer answer = SPISOR_INIT(runtime_paths_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.home = place.home;
    argument.names = &names.root;
    status = paths->resolve(paths->context, &argument, &answer, &refusal);
    paths->release_answer(paths->context, &answer);
    paths->release_refusal(paths->context, &refusal);
    return status;
}

static spisor_status resolve_places(void) {
    const runtime_places_definition *places = runtime_places_definition_of();
    runtime_variable environment[] = {LOCAL_APP_DATA, {"SPISOR_HOME", ""}};
    runtime_places_resolve_argument argument = SPISOR_INIT(runtime_places_resolve_argument);
    runtime_places_answer answer = SPISOR_INIT(runtime_places_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.home = place.home;
    argument.environment = environment;
    argument.environment_count = 2;
    status = places->resolve(places->context, &argument, &answer, &refusal);
    places->release_answer(places->context, &answer);
    places->release_refusal(places->context, &refusal);
    return status;
}

static spisor_status set_secret(void) {
    const runtime_secrets_definition *secrets = runtime_secrets_definition_of();
    runtime_secret_places places = SPISOR_INIT(runtime_secret_places);
    runtime_secrets_set_argument argument = SPISOR_INIT(runtime_secrets_set_argument);
    runtime_secrets_set_answer answer = SPISOR_INIT(runtime_secrets_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    places.secrets_file = place.secrets_file;
    places.force_file_store = 1;
    argument.places = &places;
    argument.plugin = "p";
    argument.name = "token";
    argument.value = "v1";
    status = secrets->set(secrets->context, &argument, &answer, &refusal);
    secrets->release_refusal(secrets->context, &refusal);
    return status;
}

static spisor_status set_config(void) {
    const runtime_config_definition *definition = runtime_config_definition_of();
    runtime_config_plugin plugin = SPISOR_INIT(runtime_config_plugin);
    runtime_settings_change change = SPISOR_INIT(runtime_settings_change);
    runtime_config_set_answer answer = SPISOR_INIT(runtime_config_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    plugin.home = place.home;
    plugin.portable = place.portable;
    plugin.plugin = "p";
    plugin.config = &config.root;
    change.key = "a.c";
    change.value = &value.root;
    status = definition->set(definition->context, &plugin, &change, &answer, &refusal);
    definition->release_set_answer(definition->context, &answer);
    definition->release_refusal(definition->context, &refusal);
    return status;
}

static spisor_status all_settings(void) {
    const runtime_settings_definition *settings = runtime_settings_definition_of();
    runtime_settings_scope scope = SPISOR_INIT(runtime_settings_scope);
    spisor_value_tree values = SPISOR_INIT(spisor_value_tree);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    scope.home = place.home;
    scope.portable = place.portable;
    status = settings->all(settings->context, &scope, &values, &refusal);
    settings->release_tree(settings->context, &values);
    settings->release_refusal(settings->context, &refusal);
    return status;
}

static spisor_status write_log(void) {
    const runtime_log_definition *log = runtime_log_definition_of();
    runtime_log_entry entry = SPISOR_INIT(runtime_log_entry);
    runtime_log_answer answer = SPISOR_INIT(runtime_log_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    entry.home = place.home;
    entry.portable = place.portable;
    entry.plugin = "p";
    entry.config = &config.root;
    entry.start = START;
    entry.time = TIME;
    entry.level = RUNTIME_LOG_INFO;
    entry.message = "m";
    status = log->write(log->context, &entry, &answer, &refusal);
    log->release_answer(log->context, &answer);
    log->release_refusal(log->context, &refusal);
    return status;
}

static spisor_status agree_on_copies(void) {
    const runtime_identity_definition *identity = runtime_identity_definition_of();
    runtime_identity_copy copies[3] = {{"github:1", 0, &first_file.root}, {"github:2", 0, &second_file.root},
                                       {"github:3", 1, NULL}};
    runtime_identity_agree_argument argument = SPISOR_INIT(runtime_identity_agree_argument);
    runtime_identity_agreement answer = SPISOR_INIT(runtime_identity_agreement);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.id = "github:1";
    argument.copies = copies;
    argument.copy_count = 3;
    status = identity->agree(identity->context, &argument, &answer, &refusal);
    identity->release_agreement(identity->context, &answer);
    identity->release_refusal(identity->context, &refusal);
    return status;
}

/** The file: url of a path, every byte but the plain ones percent-encoded. */
static void file_url_of(const char *path, char *url, size_t capacity) {
    size_t length = (size_t)snprintf(url, capacity, path[0] == '/' ? "file://" : "file:///");
    for (const unsigned char *at = (const unsigned char *)path; *at != 0 && length + 4 < capacity; at++) {
        unsigned char character = *at == '\\' ? '/' : *at;
        int plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || strchr("/-._~:", character) != NULL;
        length += (size_t)(plain ? snprintf(url + length, capacity - length, "%c", character)
                                 : snprintf(url + length, capacity - length, "%%%02X", character));
    }
}

static spisor_status fetch_a_file(void) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char source[1400];
    char url[4200];
    char destination[1400];
    spisor_status status;
    snprintf(source, sizeof source, "%s/settings.json", place.config_directory);
    snprintf(destination, sizeof destination, "%s/fetched/settings.json", place.base);
    file_url_of(source, url, sizeof url);
    argument.url = url;
    argument.destination = destination;
    status = fetch->get(fetch->context, &argument, &answer, &refusal);
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &refusal);
    return status;
}

static spisor_status find_on_path(void) {
    const runtime_detect_definition *detect = runtime_detect_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_detect_on_path_argument argument = SPISOR_INIT(runtime_detect_on_path_argument);
    runtime_detect_on_path_answer answer = SPISOR_INIT(runtime_detect_on_path_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.binary = "probe";
    argument.environment = environment;
    argument.environment_count = 2;
    status = detect->on_path(detect->context, &argument, &answer, &refusal);
    detect->release_answer(detect->context, &answer);
    detect->release_refusal(detect->context, &refusal);
    return status;
}

static spisor_status register_a_host(void) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    runtime_registry_provenance provenance = SPISOR_INIT(runtime_registry_provenance);
    runtime_registry_start start = SPISOR_INIT(runtime_registry_start);
    runtime_registration registration = SPISOR_INIT(runtime_registration);
    runtime_registry_register_argument argument = SPISOR_INIT(runtime_registry_register_argument);
    runtime_registry_registration_answer answer = SPISOR_INIT(runtime_registry_registration_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    provenance.method = "npm";
    provenance.version = "1.0.0";
    provenance.location = place.home;
    provenance.os = "linux";
    provenance.architecture = "x64";
    provenance.installed_at = "2026-11-02T10:00:00Z";
    provenance.options = &config.root;
    start.runtime = RUNTIME_REGISTRY_START_NATIVE;
    start.entry = "bin/host";
    registration.id = "claude";
    registration.version = "1.0.0";
    registration.home = place.home;
    registration.application = &names.root;
    registration.provenance = &provenance;
    registration.start = &start;
    argument.state = place.state;
    argument.registration = &registration;
    status = registry->register_host(registry->context, &argument, &answer, &refusal);
    registry->release_answer(registry->context, &answer);
    registry->release_refusal(registry->context, &refusal);
    return status;
}

static spisor_status apply_a_change_as(const char *host) {
    const runtime_changes_definition *changes = runtime_changes_definition_of();
    const char *const key[] = {"a", "c"};
    char file[1300];
    runtime_change change = SPISOR_INIT(runtime_change);
    runtime_changes_apply_argument argument = SPISOR_INIT(runtime_changes_apply_argument);
    runtime_changes_apply_answer answer = SPISOR_INIT(runtime_changes_apply_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    snprintf(file, sizeof file, "%s/p.json", place.config_directory);
    change.kind = RUNTIME_CHANGE_KIND_JSON_KEY;
    change.file = file;
    change.key = key;
    change.key_count = 2;
    change.value = &value.root;
    argument.state = place.state;
    argument.host = host;
    argument.change = &change;
    status = changes->apply(changes->context, &argument, &answer, &refusal);
    changes->release_refusal(changes->context, &refusal);
    return status;
}

static spisor_status apply_a_change(void) {
    return apply_a_change_as("changer");
}

/** An apply that first settles a pending record against the file, reading it to find whether its key is there. */
static spisor_status apply_after_settling(void) {
    return apply_a_change_as("settler");
}

/** Whether host's registration holds at most `count` changes. */
static int holds_at_most(const char *host, int count) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place.state, host, &registration) == RUNTIME_REGISTRY_OK);
    int held = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(registration, "changes")) <= count;
    cJSON_Delete(registration);
    return held;
}

/** Whether the file still holds what it held before the apply. */
static int file_unchanged(void) {
    char file[1300];
    size_t length = 0;
    snprintf(file, sizeof file, "%s/p.json", place.config_directory);
    uint8_t *bytes = spisor_read_file(file, &length);
    int unchanged = bytes != NULL && length == strlen(CHANGED_FILE) && memcmp(bytes, CHANGED_FILE, length) == 0;
    free(bytes);
    return unchanged;
}

/** Whether an apply after settling recorded no change of its own, the file as it was. */
static int settled_and_changed_nothing(void) {
    return file_unchanged() && holds_at_most("settler", 1);
}

/**
 * Whether an apply changed nothing: the file as it was and no change recorded.
 *
 * @implNote Not the registration's bytes: once the pending record was written, its number is never handed out again,
 * so the counter may have moved.
 */
static int changed_nothing(void) {
    return file_unchanged() && holds_at_most("changer", 0);
}

/** Whether a register left no record behind. */
static int registered_nothing(void) {
    char record[1300];
    snprintf(record, sizeof record, "%s/hosts/claude.json", place.state);
    return !spisor_file_exists(record);
}

/** Whether log's write left no line behind: no logs folder under the home at all. */
static int appended_nothing(void) {
    char logs[1200];
    snprintf(logs, sizeof logs, "%s/logs", place.home);
    return !spisor_file_exists(logs);
}

typedef int (*untouched)(void);

/**
 * Fails the operation's allocations one at a time until it runs whole, checking each failure as the file says and,
 * with `left_alone` set, that the failure changed nothing it names.
 */
static void every_failure_answers_out_of_memory(const char *label, operation run, untouched left_alone) {
    for (uint64_t attempt = 1; attempt < 100000; attempt++) {
        fresh_files();
        int64_t baseline = spisor_memory_live_blocks();
        spisor_memory_fail_at(attempt);
        spisor_status status = run();
        int failed = spisor_memory_failed();
        spisor_memory_fail_at(0);
        if (spisor_memory_live_blocks() != baseline) {
            fprintf(stderr, "%s: failing allocation %llu left %lld blocks live\n", label, (unsigned long long)attempt,
                    (long long)(spisor_memory_live_blocks() - baseline));
            check_failures++;
            return;
        }
        if (!failed) {
            if (status != SPISOR_OK) {
                fprintf(stderr, "%s: with no failure it answered %d\n", label, (int)status);
                check_failures++;
            }
            printf("%s: %llu allocations, each failure answered out of memory\n", label,
                   (unsigned long long)(attempt - 1));
            return;
        }
        if (left_alone != NULL && !left_alone()) {
            fprintf(stderr, "%s: failing allocation %llu answered out of memory after it wrote\n", label,
                    (unsigned long long)attempt);
            check_failures++;
            return;
        }
        if (status != SPISOR_OUT_OF_MEMORY) {
            fprintf(stderr, "%s: failing allocation %llu answered %d\n", label, (unsigned long long)attempt,
                    (int)status);
            check_failures++;
            return;
        }
    }
    fprintf(stderr, "%s: never ran whole\n", label);
    check_failures++;
}

int main(void) {
    spisor_json_use_library_allocator();
    snprintf(place.base, sizeof place.base, RUNTIME_SCRATCH_PATH "/out-of-memory-%d", (int)CURRENT_PROCESS_ID);
    snprintf(place.home, sizeof place.home, "%s/home", place.base);
    snprintf(place.portable, sizeof place.portable, "%s/portable", place.base);
    snprintf(place.secrets_file, sizeof place.secrets_file, "%s/state/secrets.json", place.base);
    snprintf(place.config_directory, sizeof place.config_directory, "%s/config", place.home);
    snprintf(place.programs, sizeof place.programs, "%s/programs", place.base);
    snprintf(place.state, sizeof place.state, "%s/state", place.base);
    CHECK(value_of_text("{\"cache\":\"c\"}", &names));
    CHECK(value_of_text("{\"defaults\":{\"a\":{\"d\":[1,\"two\"]}}}", &config));
    CHECK(value_of_text("{\"x\":[true,null,3]}", &value));
    CHECK(value_of_text("{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]}",
                        &first_file));
    CHECK(value_of_text("{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}", &second_file));
    every_failure_answers_out_of_memory("paths' resolve", resolve_paths, NULL);
    every_failure_answers_out_of_memory("places' resolve", resolve_places, NULL);
    every_failure_answers_out_of_memory("secrets' set", set_secret, NULL);
    every_failure_answers_out_of_memory("config's set", set_config, NULL);
    every_failure_answers_out_of_memory("settings' all", all_settings, NULL);
    every_failure_answers_out_of_memory("log's write", write_log, appended_nothing);
    every_failure_answers_out_of_memory("identity's agree", agree_on_copies, NULL);
    every_failure_answers_out_of_memory("fetch's get", fetch_a_file, NULL);
    every_failure_answers_out_of_memory("detect's on_path", find_on_path, NULL);
    every_failure_answers_out_of_memory("registry's register_host", register_a_host, registered_nothing);
    every_failure_answers_out_of_memory("changes' apply", apply_a_change, changed_nothing);
    every_failure_answers_out_of_memory("changes' apply after settling", apply_after_settling,
                                        settled_and_changed_nothing);
    spisor_value_release(&names);
    spisor_value_release(&config);
    spisor_value_release(&value);
    spisor_value_release(&first_file);
    spisor_value_release(&second_file);
    CHECK(runtime_remove_tree(place.base) == 1);
    return check_summary("out_of_memory_test");
}
