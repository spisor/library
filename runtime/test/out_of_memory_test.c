#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include "path_entry_windows_hooks.h"
#define CURRENT_PROCESS_ID _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "allocation.h"
#include "allocation_failure.h"
#include "check.h"
#include "definition_fixture.h"
#include "spisor/json_allocation.h"
#include "runtime/cache.h"
#include "runtime/catalog.h"
#include "runtime/changes.h"
#include "runtime/commands.h"
#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/install.h"
#include "runtime/lists.h"
#include "runtime/log.h"
#include "runtime/marketplace.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/registry.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
#include "cache_marker.h"
#include "catalog_hooks.h"
#include "catalog_path.h"
#include "catalog_state.h"
#include "directory_tree.h"
#include "fetch_hooks.h"
#include "host_references.h"
#include "http_fixture.h"
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
#define PACKAGES_FILE "{\"tool\":{\"brew\":{\"name\":\"tool\",\"options\":{\"tap\":\"v/t\"}}}}"
#define TIME "2026-10-27T19:34:53.123Z"
#define MARKETPLACE_COPY                                                                                           \
    "{\"format\":1,\"built\":\"2026-11-10T12:00:00Z\",\"entries\":["                                              \
    "{\"id\":\"github:1\",\"name\":\"runtime\",\"fullName\":\"spisor/runtime\",\"lastKnown\":\"spisor/runtime\"}]}"

typedef struct scratch {
    char base[1024];
    char home[1100];
    char portable[1100];
    char secrets_file[1200];
    char config_directory[1200];
    char programs[1200];
    char state[1200];
    char command_state[1200];
} scratch;

static scratch place;
static spisor_value_tree names = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree config = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree value = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree first_file = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree second_file = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree plugin_rows = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree catalog_items = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree whole_catalog = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree manifest_packages = SPISOR_INIT(spisor_value_tree);

static void write_text(const char *directory, const char *name, const char *text) {
    char path[1400];
    CHECK(spisor_make_directories(directory) == SPISOR_OK);
    snprintf(path, sizeof path, "%s/%s", directory, name);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
}

#ifdef _WIN32
static void forget_test_path_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, runtime_path_entry_test_value());
        RegCloseKey(key);
    }
}

static int test_path_value_present(void) {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, L"Environment", runtime_path_entry_test_value(), RRF_RT_ANY, NULL, NULL,
                        &size) == ERROR_SUCCESS;
}
#else
static void forget_test_path_value(void) {
}

static int test_path_value_present(void) {
    return 0;
}
#endif

/** The files every operation reads, written afresh before each attempt so no attempt sees another's writes. */
/** A bundle version installed in the cache place, with a marker that records no files. */
static void installed_bundle(void) {
    char directory[1300];
    cJSON *none = cJSON_CreateObject();
    snprintf(directory, sizeof directory, "%s/cache/bundles/a/1.0.0", place.base);
    write_text(directory, "plugin.json", "{\"importName\":\"a\",\"api\":1,\"version\":\"1.0.0\"}");
    CHECK(none != NULL && runtime_cache_write_marker(directory, none, none) == SPISOR_OK);
    cJSON_Delete(none);
}

static void fresh_files(void) {
    start_empty_directory(place.base);
    write_text(place.config_directory, "p.json", CHANGED_FILE);
    write_text(place.config_directory, "settings.json", "{\"logConsole\":true,\"ui\":{\"size\":2}}");
    write_text(place.portable, "settings.json", "{\"ui\":{\"theme\":\"dark\"}}");
    write_text(place.portable, "packages.json", PACKAGES_FILE);
    write_text(place.programs, "probe", "");
    char record[8192];
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
    snprintf(record, sizeof record,
             "{\"id\":\"reverser\",\"kind\":\"host\",\"version\":\"1\",\"home\":\"%s\",\"provenance\":"
             "{\"method\":\"npm\",\"version\":\"1\",\"location\":\"%s\",\"os\":\"linux\",\"architecture\":\"x64\","
             "\"installedAt\":\"2026-11-02T10:00:00Z\"},\"requiresCommand\":false,\"commands\":[],\"changes\":["
             "{\"sequence\":1,\"kind\":\"json-key\",\"state\":\"applied\",\"reversed\":false,"
             "\"file\":\"%s/p.json\",\"key\":[\"logging\"],\"value\":true,\"previous\":false}],\"nextSequence\":2}",
             place.home, place.home, place.config_directory);
    write_text(hosts, "reverser.json", record);
    snprintf(record, sizeof record,
             "{\"id\":\"commander\",\"kind\":\"host\",\"version\":\"1\",\"home\":\"%s\","
             "\"application\":{\"id\":\"claude\"},\"provenance\":"
             "{\"method\":\"npm\",\"version\":\"1\",\"location\":\"%s\",\"os\":\"linux\",\"architecture\":\"x64\","
             "\"installedAt\":\"2026-11-02T10:00:00Z\"},\"requiresCommand\":false,\"commands\":[{\"name\":\"cc\","
             "\"requested\":\"cc\",\"owner\":\"host\",\"state\":\"active\"}],\"changes\":[]}",
             place.home, place.home);
    write_text(hosts, "commander.json", record);
    snprintf(hosts, sizeof hosts, "%s/hosts", place.command_state);
    write_text(hosts, "commander.json", record);
    snprintf(record, sizeof record,
             "{\"id\":\"namer\",\"kind\":\"host\",\"version\":\"1\",\"home\":\"%s\",\"provenance\":"
             "{\"method\":\"npm\",\"version\":\"1\",\"location\":\"%s\",\"os\":\"linux\",\"architecture\":\"x64\","
             "\"installedAt\":\"2026-11-02T10:00:00Z\"},\"requiresCommand\":false,\"commands\":[],\"changes\":[]}",
             place.home, place.home);
    write_text(hosts, "namer.json", record);
    installed_bundle();
    forget_test_path_value();
#ifndef _WIN32
    char probe[1300];
    snprintf(probe, sizeof probe, "%s/probe", place.programs);
    CHECK(chmod(probe, 0755) == 0);
#endif
}

typedef spisor_status (*operation)(void);

static spisor_status merge_packages(void) {
    const runtime_install_definition *install = runtime_install_definition_of();
    runtime_install_packages_argument argument = SPISOR_INIT(runtime_install_packages_argument);
    runtime_install_packages_answer answer = SPISOR_INIT(runtime_install_packages_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.portable = place.portable;
    argument.item = "tool";
    argument.manifest_packages = &manifest_packages.root;
    status = install->packages(install->context, &argument, &answer, &refusal);
    CHECK(status != SPISOR_OK || (answer.package_count == 2 && answer.packages[1].options != NULL));
    install->release_packages_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    return status;
}

/** Whether the user's packages.json still holds what it held. */
static int packages_file_unchanged(void) {
    char file[1300];
    size_t length = 0;
    snprintf(file, sizeof file, "%s/packages.json", place.portable);
    uint8_t *bytes = spisor_read_file_reporting(file, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    int unchanged = bytes != NULL && length == strlen(PACKAGES_FILE) && memcmp(bytes, PACKAGES_FILE, length) == 0;
    spisor_memory_release(bytes);
    return unchanged;
}

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

/** Whether no file stands anywhere under directory, which need not exist. */
static int holds_no_file(const char *directory) {
    runtime_directory_listing listing;
    if (runtime_list_directory(directory, &listing) != SPISOR_OK) {
        return 1;
    }
    int none = 1;
    for (size_t index = 0; none && index < listing.count; index++) {
        char path[1600];
        snprintf(path, sizeof path, "%s/%s", directory, listing.entries[index].name);
        none = listing.entries[index].is_directory && holds_no_file(path);
    }
    runtime_free_directory_listing(&listing);
    return none;
}

/** Whether a get left nothing in the destination's folder, neither the file nor its partial download. */
static int fetched_nothing(void) {
    char folder[1400];
    snprintf(folder, sizeof folder, "%s/fetched", place.base);
    return holds_no_file(folder);
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

/** Reverses reverser's one json-key change, whose record carries the previous value but not its bytes. */
static spisor_status reverse_a_change(void) {
    const runtime_changes_definition *changes = runtime_changes_definition_of();
    runtime_changes_reverse_argument argument = SPISOR_INIT(runtime_changes_reverse_argument);
    runtime_changes_reverse_answer answer = SPISOR_INIT(runtime_changes_reverse_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.state = place.state;
    argument.host = "reverser";
    status = changes->reverse(changes->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && answer.reversed != 1) {
        status = SPISOR_REFUSED;
    }
    changes->release_reverse_answer(changes->context, &answer);
    changes->release_refusal(changes->context, &refusal);
    return status;
}

/**
 * Whether host's registration holds at most `count` applied changes.
 *
 * @implNote A pending record may stay: an undo that memory kept from storing its withdrawal leaves one, which the next
 * apply settles against the file.
 */
static int holds_at_most(const char *host, int count) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place.state, host, &registration) == RUNTIME_REGISTRY_OK);
    int applied = 0;
    const cJSON *change;
    cJSON_ArrayForEach(change, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(change, "state"));
        applied += state != NULL && strcmp(state, "applied") == 0;
    }
    cJSON_Delete(registration);
    return applied <= count;
}

/** Whether the file still holds what it held before the apply. */
static int file_unchanged(void) {
    char file[1300];
    size_t length = 0;
    snprintf(file, sizeof file, "%s/p.json", place.config_directory);
    uint8_t *bytes = spisor_read_file_reporting(file, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    int unchanged = bytes != NULL && length == strlen(CHANGED_FILE) && memcmp(bytes, CHANGED_FILE, length) == 0;
    spisor_memory_release(bytes);
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

static spisor_status delete_a_command(void) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_commands_delete_argument argument = SPISOR_INIT(runtime_commands_delete_argument);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.state = place.state;
    argument.host = "commander";
    argument.name = "cc";
    status = commands->remove(commands->context, &argument, &refusal);
    commands->release_refusal(commands->context, &refusal);
    return status;
}

/** Whether a delete left the command active in the stored registration. */
static int deleted_nothing(void) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place.state, "commander", &registration) == RUNTIME_REGISTRY_OK);
    const cJSON *command = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(registration, "commands"), 0);
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "state"));
    int active = state != NULL && strcmp(state, "active") == 0;
    cJSON_Delete(registration);
    return active;
}

static spisor_status list_plugins(void) {
    const runtime_lists_definition *lists = runtime_lists_definition_of();
    runtime_lists_plugins_argument argument = SPISOR_INIT(runtime_lists_plugins_argument);
    runtime_list answer = SPISOR_INIT(runtime_list);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.installed = &plugin_rows.root;
    argument.detected = &plugin_rows.root;
    argument.available = &plugin_rows.root;
    status = lists->plugins(lists->context, &argument, &answer, &refusal);
    lists->release_list(lists->context, &answer);
    lists->release_refusal(lists->context, &refusal);
    return status;
}

static spisor_status list_applications(void) {
    const runtime_lists_definition *lists = runtime_lists_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_list_detection detections[] = {{"opencode-host", 1, NULL}};
    runtime_lists_applications_argument argument = SPISOR_INIT(runtime_lists_applications_argument);
    runtime_list answer = SPISOR_INIT(runtime_list);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.items = &catalog_items.root;
    argument.state = place.state;
    argument.environment = environment;
    argument.environment_count = 2;
    argument.detections = detections;
    argument.detection_count = 1;
    status = lists->applications(lists->context, &argument, &answer, &refusal);
    lists->release_list(lists->context, &answer);
    lists->release_refusal(lists->context, &refusal);
    return status;
}

static spisor_status remember_a_name(void) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_remember_argument argument = SPISOR_INIT(runtime_marketplace_remember_argument);
    runtime_marketplace_remember_answer answer = SPISOR_INIT(runtime_marketplace_remember_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.state = place.state;
    argument.marketplace = "spisor";
    argument.name = "calendar";
    argument.id = "github:7";
    status = marketplace->remember(marketplace->context, &argument, &answer, &refusal);
    marketplace->release_remember_answer(marketplace->context, &answer);
    marketplace->release_refusal(marketplace->context, &refusal);
    return status;
}

static void a_copy_to_fetch(void) {
    write_text(place.base, "marketplace.json", MARKETPLACE_COPY);
}

/** Reads the marketplace by fetching its copy into a destination, which the read removes once it has read it. */
static spisor_status read_a_fetched_copy(void) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_read_argument argument = SPISOR_INIT(runtime_marketplace_read_argument);
    runtime_marketplace_copy_answer answer = SPISOR_INIT(runtime_marketplace_copy_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char source[1300];
    char url[4200];
    char destination[1300];
    snprintf(source, sizeof source, "%s/marketplace.json", place.base);
    snprintf(destination, sizeof destination, "%s/downloaded/marketplace.json", place.base);
    file_url_of(source, url, sizeof url);
    argument.id = "spisor";
    argument.url = url;
    argument.destination = destination;
    spisor_status status = marketplace->read(marketplace->context, &argument, &answer, &refusal);
    marketplace->release_copy_answer(marketplace->context, &answer);
    marketplace->release_refusal(marketplace->context, &refusal);
    return status;
}

static int left_no_download(void) {
    char folder[1300];
    snprintf(folder, sizeof folder, "%s/downloaded", place.base);
    return holds_no_file(folder);
}

static spisor_status read_a_copy(void) {
    const runtime_marketplace_definition *marketplace = runtime_marketplace_definition_of();
    runtime_marketplace_read_argument argument = SPISOR_INIT(runtime_marketplace_read_argument);
    runtime_marketplace_copy_answer answer = SPISOR_INIT(runtime_marketplace_copy_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.id = "spisor";
    argument.url = "https://example.com/marketplace.json";
    argument.text = MARKETPLACE_COPY;
    status = marketplace->read(marketplace->context, &argument, &answer, &refusal);
    marketplace->release_copy_answer(marketplace->context, &answer);
    marketplace->release_refusal(marketplace->context, &refusal);
    return status;
}

/** Whether a remember left no record of names behind. */
static int remembered_nothing(void) {
    char record[1300];
    snprintf(record, sizeof record, "%s/marketplace-names.json", place.state);
    return spisor_probe_file(record) != SPISOR_FILE_PRESENT;
}

static spisor_status list_installed_versions(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    runtime_cache_place_argument argument = SPISOR_INIT(runtime_cache_place_argument);
    runtime_cache_installed_answer answer = SPISOR_INIT(runtime_cache_installed_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    snprintf(cache_place, sizeof cache_place, "%s/cache", place.base);
    argument.cache = cache_place;
    status = cache->installed(cache->context, &argument, &answer, &refusal);
    CHECK(status != SPISOR_OK || (answer.bundle_count == 1 && answer.bundles[0].version_count == 1));
    cache->release_installed_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static spisor_status merge_catalogs(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    runtime_catalog_reading readings[2] = {{{RUNTIME_CATALOG_LOCAL, "/first.json"}, &whole_catalog.root},
                                           {{RUNTIME_CATALOG_HTTPS, "https://example.com/c.json"}, &whole_catalog.root}};
    runtime_catalog_merge_argument argument = SPISOR_INIT(runtime_catalog_merge_argument);
    runtime_catalog_merge_answer answer = SPISOR_INIT(runtime_catalog_merge_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.catalogs = readings;
    argument.catalog_count = 2;
    status = catalog->merge(catalog->context, &argument, &answer, &refusal);
    CHECK(status != SPISOR_OK || answer.shadowed_count == 2);
    catalog->release_merge_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

/** Whether a register left no record behind. */
static int registered_nothing(void) {
    char record[1300];
    snprintf(record, sizeof record, "%s/hosts/claude.json", place.state);
    return spisor_probe_file(record) != SPISOR_FILE_PRESENT;
}

/** Whether log's write left no line behind: no logs folder under the home at all. */
static int appended_nothing(void) {
    char logs[1200];
    snprintf(logs, sizeof logs, "%s/logs", place.home);
    return spisor_probe_file(logs) != SPISOR_FILE_PRESENT;
}

static void command_argument_of(runtime_command_argument *argument, const char *name, char *target, size_t capacity,
                                const runtime_variable *environment) {
    snprintf(target, capacity, "%s/probe", place.programs);
    argument->state = place.command_state;
    argument->host = "namer";
    argument->home = place.home;
    argument->name = name;
    argument->target = target;
    argument->environment = environment;
    argument->environment_count = 2;
}

/** Asks for cc as namer while commander holds an active cc, so the answer falls back to cc2. */
static spisor_status request_a_command(void) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char target[1300];
    spisor_status status;
    command_argument_of(&argument, "cc", target, sizeof target, environment);
    status = commands->request(commands->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.name == NULL || strcmp(answer.name, "cc2") != 0)) {
        status = SPISOR_REFUSED;
    }
    commands->release_answer(commands->context, &answer);
    commands->release_refusal(commands->context, &refusal);
    return status;
}

static spisor_status add_a_command(void) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char target[1300];
    spisor_status status;
    command_argument_of(&argument, "dd", target, sizeof target, environment);
    status = commands->add(commands->context, &argument, &answer, &refusal);
    commands->release_answer(commands->context, &answer);
    commands->release_refusal(commands->context, &refusal);
    return status;
}

/** Gives namer the command zz through add, so its shim and PATH entry carry change records. */
static void add_zz(void) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_command_argument argument = SPISOR_INIT(runtime_command_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char target[1300];
    command_argument_of(&argument, "zz", target, sizeof target, environment);
    CHECK(commands->add(commands->context, &argument, &answer, &refusal) == SPISOR_OK);
    commands->release_answer(commands->context, &answer);
    commands->release_refusal(commands->context, &refusal);
}

static spisor_status rename_zz_to(const char *to) {
    const runtime_commands_definition *commands = runtime_commands_definition_of();
    runtime_variable environment[] = {{"PATH", place.programs}, {"PATHEXT", ".EXE"}};
    runtime_commands_rename_argument argument = SPISOR_INIT(runtime_commands_rename_argument);
    runtime_command_answer answer = SPISOR_INIT(runtime_command_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status;
    argument.state = place.command_state;
    argument.host = "namer";
    argument.from = "zz";
    argument.to = to;
    argument.environment = environment;
    argument.environment_count = 2;
    status = commands->rename(commands->context, &argument, &answer, &refusal);
    commands->release_answer(commands->context, &answer);
    commands->release_refusal(commands->context, &refusal);
    return status;
}

static spisor_status rename_a_command(void) {
    return rename_zz_to("dd");
}

/** Answers OK when the rename is refused, as it must be while commander holds an active cc. */
static spisor_status rename_onto_a_held_name(void) {
    spisor_status status = rename_zz_to("cc");
    return status == SPISOR_REFUSED ? SPISOR_OK : status == SPISOR_OK ? SPISOR_REFUSED : status;
}

typedef int (*untouched)(void);

/** Whether namer's registration holds exactly one active command, named `name`. */
static int namer_holds_only(const char *name) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place.command_state, "namer", &registration) == RUNTIME_REGISTRY_OK);
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(registration, "commands");
    const cJSON *command = cJSON_GetArrayItem(commands, 0);
    const char *held = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "name"));
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(command, "state"));
    int only = cJSON_GetArraySize(commands) == 1 && held != NULL && strcmp(held, name) == 0 && state != NULL &&
               strcmp(state, "active") == 0;
    cJSON_Delete(registration);
    return only;
}

static int names_file(const char *path, const char *name) {
    size_t path_length = path == NULL ? 0 : strlen(path);
    size_t name_length = strlen(name);
    return path_length > name_length && strcmp(path + path_length - name_length, name) == 0 &&
           (path[path_length - name_length - 1] == '/' || path[path_length - name_length - 1] == '\\');
}

/**
 * Whether namer holds an unreversed record of `kind` whose `member` names `file`, or any of that kind when `file` is
 * NULL.
 */
static int recorded(const cJSON *registration, const char *kind, const char *member, const char *file) {
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        const char *entry_kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "kind"));
        if (entry_kind != NULL && strcmp(entry_kind, kind) == 0 &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reversed")) &&
            (file == NULL || names_file(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, member)), file))) {
            return 1;
        }
    }
    return 0;
}

/** Whether each profile under the home that names the shim folder has an unreversed profile-line record. */
static int every_profile_line_is_recorded(const cJSON *registration) {
    static const char *const profiles[] = {".profile", ".bashrc", ".zshrc", ".zprofile"};
    char shims[1300];
    int all = 1;
    snprintf(shims, sizeof shims, "%s/bin", place.command_state);
    for (size_t index = 0; all && index < sizeof profiles / sizeof profiles[0]; index++) {
        char profile[1400];
        size_t length = 0;
        snprintf(profile, sizeof profile, "%s/%s", place.home, profiles[index]);
        uint8_t *bytes = spisor_read_file_reporting(profile, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
        int names_shims = bytes != NULL && strstr((const char *)bytes, shims) != NULL;
        spisor_memory_release(bytes);
        all = !names_shims || recorded(registration, "profile-line", "file", profiles[index]);
    }
    return all;
}

/** Whether every shim file, the PATH entry and each profile line on disk is one namer's registration records. */
static int every_change_is_recorded(void) {
    char shims[1300];
    runtime_directory_listing listing;
    cJSON *registration = NULL;
    snprintf(shims, sizeof shims, "%s/bin", place.command_state);
    CHECK(runtime_registry_load(place.command_state, "namer", &registration) == RUNTIME_REGISTRY_OK);
    int all = runtime_list_directory(shims, &listing) == SPISOR_OK;
    for (size_t index = 0; all && index < listing.count; index++) {
        all = recorded(registration, "file", "path", listing.entries[index].name);
    }
    runtime_free_directory_listing(&listing);
    all = all && (!test_path_value_present() || recorded(registration, "path-entry", NULL, NULL)) &&
          every_profile_line_is_recorded(registration);
    cJSON_Delete(registration);
    return all;
}

static int is_shim_of_zz(const char *file_name) {
    return strncmp(file_name, "zz", 2) == 0 && (file_name[2] == '\0' || file_name[2] == '.');
}

/** Whether the shim folder holds zz's shim files and nothing a failed call wrote beside them. */
static int shims_are_only_zz(void) {
    char shims[1300];
    runtime_directory_listing listing;
    snprintf(shims, sizeof shims, "%s/bin", place.command_state);
    int only = runtime_list_directory(shims, &listing) == SPISOR_OK;
    for (size_t index = 0; only && index < listing.count; index++) {
        only = is_shim_of_zz(listing.entries[index].name);
    }
    runtime_free_directory_listing(&listing);
    return only;
}

static int kept_zz_and_recorded_all(void) {
    return namer_holds_only("zz") && every_change_is_recorded() && shims_are_only_zz();
}

/** Whether a rename took effect: namer's one active command is dd. */
static int renamed_to_dd(void) {
    return namer_holds_only("dd");
}


/**
 * One operation to fail allocation by allocation. `prepare` runs after the fresh files and before the first failure;
 * `left_alone` names what a failure must not have changed; `took_effect`, when it holds after a failure, means the
 * call completed and must answer OK; `answer_checked` accepts OK, the operation having already turned a wrong OK
 * answer into another status.
 */
typedef struct failure_case {
    const char *label;
    operation run;
    void (*prepare)(void);
    untouched left_alone;
    untouched took_effect;
    int answer_checked;
} failure_case;

/** Whether one failed attempt answered as the case requires, reporting it when not. */
static int answered_as_required(const failure_case *each, uint64_t attempt, spisor_status status) {
    if (each->took_effect != NULL && each->took_effect()) {
        if (status == SPISOR_OK && every_change_is_recorded()) {
            return 1;
        }
        fprintf(stderr, "%s: failing allocation %llu took effect and answered %d, or left a change unrecorded\n",
                each->label,
                (unsigned long long)attempt, (int)status);
        return 0;
    }
    if (each->answer_checked && status == SPISOR_OK) {
        return 1;
    }
    if (each->left_alone != NULL && !each->left_alone()) {
        fprintf(stderr, "%s: failing allocation %llu answered out of memory after it wrote\n", each->label,
                (unsigned long long)attempt);
        return 0;
    }
    if (status != SPISOR_OUT_OF_MEMORY) {
        fprintf(stderr, "%s: failing allocation %llu answered %d\n", each->label, (unsigned long long)attempt,
                (int)status);
        return 0;
    }
    return 1;
}

/**
 * Fails the operation's allocations one at a time until it runs whole, checking each failure as the file says and
 * as the case adds.
 */
/** The failures a run checks, from the third and fourth arguments; a last of 0 checks until the case runs whole. */
static uint64_t first_attempt = 1;
static uint64_t last_attempt = 0;

static void every_failure_of_case(const failure_case *each, allocation_failure_mode mode) {
    const char *label = each->label;
    uint64_t attempt = first_attempt;
    for (; attempt < 100000 && (last_attempt == 0 || attempt <= last_attempt); attempt++) {
        fresh_files();
        if (each->prepare != NULL) {
            each->prepare();
        }
        int64_t baseline = spisor_memory_live_blocks();
        fail_allocation(mode, attempt);
        spisor_status status = each->run();
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
            printf("%s%s: %llu allocations, each failure answered out of memory\n", label,
                   allocation_failure_mode_name(mode), (unsigned long long)(attempt - 1));
            return;
        }
        if (!answered_as_required(each, attempt, status)) {
            check_failures++;
            return;
        }
    }
    if (last_attempt != 0 && attempt > last_attempt) {
        printf("%s%s: failures %llu to %llu answered out of memory\n", label, allocation_failure_mode_name(mode),
               (unsigned long long)first_attempt, (unsigned long long)last_attempt);
        return;
    }
    fprintf(stderr, "%s: never ran whole\n", label);
    check_failures++;
}

/** A label the commands, cache and catalog cases are narrowed to, from the second argument; empty runs them all. */
static const char *only_label = "";

static void every_failure_of(const failure_case *each) {
    if (strstr(each->label, only_label) == NULL) {
        return;
    }
    for (allocation_failure_mode mode = FAIL_ONE_ALLOCATION; mode < ALLOCATION_FAILURE_MODES; mode++) {
        every_failure_of_case(each, mode);
    }
}

static void every_failure_answers_out_of_memory(const char *label, operation run, untouched left_alone) {
    failure_case each = {label, run, NULL, left_alone, NULL, 0};
    every_failure_of(&each);
}

static void every_command_failure(const char *label, operation run, untouched took_effect) {
    failure_case each = {label, run, add_zz, kept_zz_and_recorded_all, took_effect, 0};
    every_failure_of(&each);
}

static void every_failure_after_preparing(const char *label, operation run, void (*prepare)(void),
                                          untouched left_alone) {
    failure_case each = {label, run, prepare, left_alone, NULL, 0};
    every_failure_of(&each);
}

static void every_service_but_commands(void) {
    every_failure_answers_out_of_memory("paths' resolve", resolve_paths, NULL);
    every_failure_answers_out_of_memory("places' resolve", resolve_places, NULL);
    every_failure_answers_out_of_memory("secrets' set", set_secret, NULL);
    every_failure_answers_out_of_memory("config's set", set_config, NULL);
    every_failure_answers_out_of_memory("settings' all", all_settings, NULL);
    every_failure_answers_out_of_memory("log's write", write_log, appended_nothing);
    every_failure_answers_out_of_memory("identity's agree", agree_on_copies, NULL);
    every_failure_answers_out_of_memory("fetch's get", fetch_a_file, fetched_nothing);
    every_failure_answers_out_of_memory("detect's on_path", find_on_path, NULL);
    every_failure_answers_out_of_memory("registry's register_host", register_a_host, registered_nothing);
    every_failure_answers_out_of_memory("changes' apply", apply_a_change, changed_nothing);
    every_failure_answers_out_of_memory("changes' apply after settling", apply_after_settling,
                                        settled_and_changed_nothing);
    every_failure_answers_out_of_memory("changes' reverse", reverse_a_change, NULL);
    every_failure_answers_out_of_memory("lists' plugins", list_plugins, NULL);
    every_failure_answers_out_of_memory("lists' applications", list_applications, NULL);
    every_failure_answers_out_of_memory("marketplace's remember", remember_a_name, remembered_nothing);
    every_failure_answers_out_of_memory("marketplace's read", read_a_copy, NULL);
    every_failure_after_preparing("marketplace's read to a destination", read_a_fetched_copy, a_copy_to_fetch,
                                  left_no_download);
    every_failure_answers_out_of_memory("cache's installed", list_installed_versions, NULL);
    every_failure_answers_out_of_memory("catalog's merge", merge_catalogs, NULL);
    every_failure_answers_out_of_memory("install's packages", merge_packages, packages_file_unchanged);
}

#define CATALOG_INDEX                                                                                              \
    "{\"format\":1,\"items\":{\"a\":{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{" \
    "\"plugin.json\":{\"url\":\"a/1.0.0/plugin.json\",\"sha256\":\""                                              \
    "0000000000000000000000000000000000000000000000000000000000000000\",\"size\":1}}}}}}}"
#define HELLO_SHA256 "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"

#define WRONG_ANSWER ((spisor_status)99)

static spisor_value_tree install_item = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree short_item = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree catalog_settings = SPISOR_INIT(spisor_value_tree);
static spisor_value_tree catalog_index = SPISOR_INIT(spisor_value_tree);

static void under_base(char *path, size_t capacity, const char *rest) {
    snprintf(path, capacity, "%s/%s", place.base, rest);
}

/** @implNote Asked without allocating, so a check a case makes inside its run never meets the failing allocation. */
static int exists_at(const char *path) {
#if defined(_WIN32)
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat status;
    return stat(path, &status) == 0;
#endif
}

static int exists_under_base(const char *rest) {
    char path[1400];
    under_base(path, sizeof path, rest);
    return exists_at(path);
}

static void marked_version(const char *version) {
    char directory[1300];
    cJSON *none = cJSON_CreateObject();
    snprintf(directory, sizeof directory, "%s/cache/bundles/a/%s", place.base, version);
    write_text(directory, "plugin.json", "{\"importName\":\"a\",\"api\":1}");
    CHECK(none != NULL && runtime_cache_write_marker(directory, none, none) == SPISOR_OK);
    cJSON_Delete(none);
}

/** a@1.0.0, a@1.1.0 and a@2.0.0 installed, and a host's run naming a@1.1.0, so a sweep removes 1.0.0 alone. */
static void three_versions_one_referenced(void) {
    char bundles[1300];
    marked_version("1.1.0");
    marked_version("2.0.0");
    under_base(bundles, sizeof bundles, "sweep-state/bundles");
    write_text(bundles, "claude-code.json", "{\"run\":{\"a\":\"1.1.0\"},\"next\":{}}");
}

static int kept_the_referenced_and_the_newest(void) {
    return exists_under_base("cache/bundles/a/1.1.0/plugin.json") &&
           exists_under_base("cache/bundles/a/2.0.0/plugin.json");
}

static spisor_status sweep_the_cache(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    char state_place[1100];
    runtime_cache_sweep_argument argument = SPISOR_INIT(runtime_cache_sweep_argument);
    runtime_cache_sweep_answer answer = SPISOR_INIT(runtime_cache_sweep_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    under_base(state_place, sizeof state_place, "sweep-state");
    argument.cache = cache_place;
    argument.state = state_place;
    spisor_status status = cache->sweep(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.removed_count != 1 || exists_under_base("cache/bundles/a/1.0.0/plugin.json") ||
                                !kept_the_referenced_and_the_newest())) {
        status = WRONG_ANSWER;
    }
    cache->release_sweep_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static spisor_status check_a_version(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char directory[1300];
    runtime_cache_check_argument argument = SPISOR_INIT(runtime_cache_check_argument);
    runtime_cache_check_answer answer = SPISOR_INIT(runtime_cache_check_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(directory, sizeof directory, "cache/bundles/a/1.0.0");
    argument.directory = directory;
    spisor_status status = cache->check(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && answer.verdict != RUNTIME_CACHE_INTACT) {
        status = WRONG_ANSWER;
    }
    cache->release_check_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static spisor_status list_damaged_versions(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    runtime_cache_place_argument argument = SPISOR_INIT(runtime_cache_place_argument);
    runtime_cache_damaged_answer answer = SPISOR_INIT(runtime_cache_damaged_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.cache = cache_place;
    spisor_status status = cache->damaged(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && answer.damaged_count != 0) {
        status = WRONG_ANSWER;
    }
    cache->release_damaged_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static spisor_status locate_a_plugin(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    runtime_item_version plugins[1] = {{"a", "1.0.0"}};
    runtime_cache_locate_argument argument = SPISOR_INIT(runtime_cache_locate_argument);
    runtime_cache_locate_answer answer = SPISOR_INIT(runtime_cache_locate_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.cache = cache_place;
    argument.plugins = plugins;
    argument.plugin_count = 1;
    spisor_status status = cache->locate(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.located_count != 1 || answer.problem_count != 0)) {
        status = WRONG_ANSWER;
    }
    cache->release_locate_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static spisor_status name_the_platform(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    runtime_cache_platform_answer answer = SPISOR_INIT(runtime_cache_platform_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_status status = cache->platform(cache->context, &answer, &refusal);
    if (status == SPISOR_OK && (answer.platform == NULL || answer.platform[0] == 0)) {
        status = WRONG_ANSWER;
    }
    cache->release_platform_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static void a_bundle_to_deploy(void) {
    char bundle[1300];
    under_base(bundle, sizeof bundle, "bundle");
    write_text(bundle, "plugin.json", "{\"importName\":\"greeter\",\"api\":1,\"entry\":\"index.js\"}");
    write_text(bundle, "index.js", "export default {};");
}

static int deployed_nothing(void) {
    return !exists_under_base("deep/plugins/greeter.json") && !exists_under_base("deep/plugins/greeter.js") &&
           !exists_under_base("deep/plugins/greeter_identity.json");
}

/** Whether a deploy that failed left neither its files nor the folders it made for pluginDir. */
static int deployed_nothing_and_made_no_folder(void) {
    return deployed_nothing() && !exists_under_base("deep");
}

static spisor_status deploy_a_bundle(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char bundle[1300];
    char plugins[1300];
    runtime_cache_deploy_argument argument = SPISOR_INIT(runtime_cache_deploy_argument);
    runtime_cache_deploy_answer answer = SPISOR_INIT(runtime_cache_deploy_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(bundle, sizeof bundle, "bundle");
    under_base(plugins, sizeof plugins, "deep/plugins");
    argument.directory = bundle;
    argument.plugin_directory = plugins;
    argument.id = "github:5";
    argument.name = "greeter";
    spisor_status status = cache->deploy(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (deployed_nothing() || answer.import_name == NULL ||
                                strcmp(answer.import_name, "greeter") != 0)) {
        status = WRONG_ANSWER;
    }
    cache->release_deploy_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

static void a_file_to_install(void) {
    write_text(place.base, "source.txt", "hello");
}

static int installed_no_marker(void) {
    return !exists_under_base("cache/bundles/b/1.0.0/.installed.json");
}

static spisor_status install_a_version(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    runtime_cache_install_argument argument = SPISOR_INIT(runtime_cache_install_argument);
    runtime_cache_install_answer answer = SPISOR_INIT(runtime_cache_install_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.cache = cache_place;
    argument.id = "b";
    argument.item = &install_item.root;
    argument.version = "1.0.0";
    argument.platform = "linux-x64";
    spisor_status status = cache->install(cache->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (installed_no_marker() || answer.fetched_count != 1)) {
        status = WRONG_ANSWER;
    }
    cache->release_install_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status;
}

/** Installs an item whose one file serves fewer bytes than the catalog says: OK when that is refused. */
static spisor_status install_a_short_file(void) {
    const runtime_cache_definition *cache = runtime_cache_definition_of();
    char cache_place[1100];
    runtime_cache_install_argument argument = SPISOR_INIT(runtime_cache_install_argument);
    runtime_cache_install_answer answer = SPISOR_INIT(runtime_cache_install_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.cache = cache_place;
    argument.id = "b";
    argument.item = &short_item.root;
    argument.version = "1.0.0";
    argument.platform = "linux-x64";
    spisor_status status = cache->install(cache->context, &argument, &answer, &refusal);
    cache->release_install_answer(cache->context, &answer);
    cache->release_refusal(cache->context, &refusal);
    return status == SPISOR_REFUSED ? SPISOR_OK : status == SPISOR_OK ? WRONG_ANSWER : status;
}

static int stored_no_short_file(void) {
    return !exists_under_base("cache/bundles/b/1.0.0/p.js");
}

static int holds_one_item(const spisor_value *catalog) {
    const spisor_value *items = spisor_value_member(catalog, "items", 5);
    return items != NULL && items->count == 1;
}

static spisor_status read_a_catalog(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    runtime_catalog_read_argument argument = SPISOR_INIT(runtime_catalog_read_argument);
    runtime_catalog_answer answer = SPISOR_INIT(runtime_catalog_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.text = CATALOG_INDEX;
    argument.origin = "https://catalog.test/index.json";
    spisor_status status = catalog->read(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && !holds_one_item(answer.catalog)) {
        status = WRONG_ANSWER;
    }
    catalog->release_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

static spisor_status list_catalog_sources(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    runtime_catalog_sources_argument argument = SPISOR_INIT(runtime_catalog_sources_argument);
    runtime_catalog_sources_answer answer = SPISOR_INIT(runtime_catalog_sources_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.settings = &catalog_settings.root;
    spisor_status status = catalog->sources(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.source_count != 2 || answer.skipped_count != 1)) {
        status = WRONG_ANSWER;
    }
    catalog->release_sources_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

static char catalog_state_place[1400];

/** The local index, and where its check keeps state, found here so a run's own check never allocates. */
static void a_local_index(void) {
    char index[1300];
    char cache_place[1100];
    write_text(place.base, "index.json", CATALOG_INDEX);
    under_base(index, sizeof index, "index.json");
    under_base(cache_place, sizeof cache_place, "cache");
    char *location = runtime_catalog_resolve_path(index);
    char *state = location == NULL ? NULL : runtime_catalog_state_path(cache_place, location);
    CHECK(state != NULL);
    snprintf(catalog_state_place, sizeof catalog_state_place, "%s", state == NULL ? "" : state);
    spisor_memory_release(state);
    spisor_memory_release(location);
}

static void local_index_source(runtime_catalog_source *source, char *path, size_t capacity) {
    under_base(path, capacity, "index.json");
    source->kind = RUNTIME_CATALOG_LOCAL;
    source->location = path;
}

static int kept_no_catalog_state(void) {
    return !exists_at(catalog_state_place);
}

static spisor_status check_a_catalog(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    char index[1300];
    char cache_place[1100];
    runtime_catalog_source source;
    runtime_catalog_check_argument argument = SPISOR_INIT(runtime_catalog_check_argument);
    runtime_catalog_check_answer answer = SPISOR_INIT(runtime_catalog_check_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    local_index_source(&source, index, sizeof index);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.source = &source;
    argument.cache = cache_place;
    argument.now = "2026-11-03T08:00:00.000Z";
    argument.interval_hours = 24;
    spisor_status status = catalog->check(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.kind != RUNTIME_CATALOG_CHANGED || !holds_one_item(answer.catalog) ||
                                kept_no_catalog_state())) {
        status = WRONG_ANSWER;
    }
    catalog->release_check_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

#define REMOTE_ORIGIN "https://catalog.test"
#define REMOTE_INDEX_URL REMOTE_ORIGIN "/index.json"

static char remote_redirect[4200];

/** A remote index whose requests the catalog redirects to the local index, and where its check keeps state. */
static void a_remote_shaped_index(void) {
    char cache_place[1100];
    write_text(place.base, "index.json", CATALOG_INDEX);
    under_base(cache_place, sizeof cache_place, "cache");
    file_url_of(place.base, remote_redirect, sizeof remote_redirect);
    runtime_catalog_redirect_requests(REMOTE_ORIGIN, remote_redirect);
    char *state = runtime_catalog_state_path(cache_place, REMOTE_INDEX_URL);
    CHECK(state != NULL);
    snprintf(catalog_state_place, sizeof catalog_state_place, "%s", state == NULL ? "" : state);
    spisor_memory_release(state);
}

/** Whether a check left nothing where it keeps state, neither the state nor its download. */
static int kept_nothing_beside_catalog_state(void) {
    char folder[1400];
    snprintf(folder, sizeof folder, "%s", catalog_state_place);
    char *separator = strrchr(folder, '/');
    char *backslash = strrchr(folder, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    if (separator != NULL) {
        *separator = '\0';
    }
    return holds_no_file(folder);
}

static spisor_status check_a_remote_shaped_catalog(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    char cache_place[1100];
    runtime_catalog_source source;
    runtime_catalog_check_argument argument = SPISOR_INIT(runtime_catalog_check_argument);
    runtime_catalog_check_answer answer = SPISOR_INIT(runtime_catalog_check_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    memset(&source, 0, sizeof source);
    source.kind = RUNTIME_CATALOG_HTTPS;
    source.location = REMOTE_INDEX_URL;
    under_base(cache_place, sizeof cache_place, "cache");
    argument.source = &source;
    argument.cache = cache_place;
    argument.now = "2026-11-03T08:00:00.000Z";
    argument.interval_hours = 24;
    spisor_status status = catalog->check(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.kind != RUNTIME_CATALOG_CHANGED || !holds_one_item(answer.catalog))) {
        status = WRONG_ANSWER;
    }
    catalog->release_check_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

static spisor_status read_the_current_catalog(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    char index[1300];
    char cache_place[1100];
    runtime_catalog_source source;
    runtime_catalog_current_argument argument = SPISOR_INIT(runtime_catalog_current_argument);
    runtime_catalog_answer answer = SPISOR_INIT(runtime_catalog_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    local_index_source(&source, index, sizeof index);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.sources = &source;
    argument.source_count = 1;
    argument.cache = cache_place;
    spisor_status status = catalog->current(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && !holds_one_item(answer.catalog)) {
        status = WRONG_ANSWER;
    }
    catalog->release_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

static spisor_status resolve_versions(void) {
    const runtime_catalog_definition *catalog = runtime_catalog_definition_of();
    const char *versions[1] = {"1.0.0"};
    runtime_catalog_enabled enabled[1] = {{"a", NULL}};
    runtime_catalog_installed installed[1] = {{"a", versions, 1}};
    runtime_catalog_resolve_argument argument = SPISOR_INIT(runtime_catalog_resolve_argument);
    runtime_catalog_resolve_answer answer = SPISOR_INIT(runtime_catalog_resolve_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    argument.catalog = &catalog_index.root;
    argument.enabled = enabled;
    argument.enabled_count = 1;
    argument.installed = installed;
    argument.installed_count = 1;
    argument.kind = RUNTIME_CATALOG_PLUGIN;
    spisor_status status = catalog->resolve(catalog->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (answer.run_count != 1 || answer.fetch_count != 0)) {
        status = WRONG_ANSWER;
    }
    catalog->release_resolve_answer(catalog->context, &answer);
    catalog->release_refusal(catalog->context, &refusal);
    return status;
}

static void every_checked_failure(const char *label, operation run, void (*prepare)(void), untouched left_alone) {
    failure_case each = {label, run, prepare, left_alone, NULL, 1};
    every_failure_of(&each);
}

static void every_cache_and_catalog_operation(void) {
    every_checked_failure("cache's sweep", sweep_the_cache, three_versions_one_referenced,
                          kept_the_referenced_and_the_newest);
    every_checked_failure("cache's check", check_a_version, NULL, NULL);
    every_checked_failure("cache's damaged", list_damaged_versions, NULL, NULL);
    every_checked_failure("cache's locate", locate_a_plugin, NULL, NULL);
    every_checked_failure("cache's platform", name_the_platform, NULL, NULL);
    every_checked_failure("cache's deploy", deploy_a_bundle, a_bundle_to_deploy, deployed_nothing_and_made_no_folder);
    every_checked_failure("cache's install", install_a_version, a_file_to_install, installed_no_marker);
    every_checked_failure("cache's install of the wrong length", install_a_short_file, a_file_to_install,
                          stored_no_short_file);
    every_checked_failure("catalog's read", read_a_catalog, NULL, NULL);
    every_checked_failure("catalog's sources", list_catalog_sources, NULL, NULL);
    every_checked_failure("catalog's check", check_a_catalog, a_local_index, kept_no_catalog_state);
    every_checked_failure("catalog's check of a remote-shaped index", check_a_remote_shaped_catalog,
                          a_remote_shaped_index, kept_nothing_beside_catalog_state);
    runtime_catalog_redirect_requests(NULL, NULL);
    every_checked_failure("catalog's current", read_the_current_catalog, a_local_index, NULL);
    every_checked_failure("catalog's resolve", resolve_versions, NULL, NULL);
}

#define PLUGIN_MANIFEST "{\"importName\":\"greeter\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"dist/index.js\"}"
#define PLUGIN_ENTRY "export default {};"
#define RELEASE_INDEX                                                                                              \
    "{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"commit\":\"abc123\",\"files\":{"        \
    "\"plugin.json\":{\"url\":\"plugin.json\",\"sha256\":"                                                         \
    "\"6ef2fa406ca66093ba8b6348f8b0547fc59e0d34de3bcddee40baaa4ece95cb3\",\"size\":74},"                         \
    "\"dist/index.js\":{\"url\":\"index.js\",\"sha256\":"                                                          \
    "\"9f085b1079ab38f776bbb3930dfd067a838ca3e0483aff8625f88837e8ed964c\",\"size\":18}}}}}"

static char by_id_api_base[64];

static void a_release_to_install(void) {
    char release[1300];
    under_base(release, sizeof release, "release");
    write_text(release, "plugin.json", PLUGIN_MANIFEST);
    write_text(release, "index.js", PLUGIN_ENTRY);
    write_text(release, "weivel-index.json", RELEASE_INDEX);
}

static int installed_by_id(void) {
    return exists_under_base("plugins/greeter.json") && exists_under_base("cache/identities/github+5.json");
}

static spisor_status install_by_id(void) {
    const runtime_install_definition *install = runtime_install_definition_of();
    char cache_place[1100];
    char plugins[1100];
    runtime_install_by_id_argument argument = SPISOR_INIT(runtime_install_by_id_argument);
    runtime_install_by_id_answer answer = SPISOR_INIT(runtime_install_by_id_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    under_base(cache_place, sizeof cache_place, "cache");
    under_base(plugins, sizeof plugins, "plugins");
    argument.id = "github:5";
    argument.cache = cache_place;
    argument.plugin_directory = plugins;
    argument.api_base = by_id_api_base;
    spisor_status status = install->by_id(install->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && (!installed_by_id() || answer.version == NULL || strcmp(answer.version, "1.0.0") != 0)) {
        status = WRONG_ANSWER;
    }
    install->release_by_id_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    return status;
}

/** The release the loopback fake answers for github:5, its index a file under the base. */
static void release_json_of(char *json, size_t capacity) {
    char index[1300];
    char url[4200];
    under_base(index, sizeof index, "release/weivel-index.json");
    file_url_of(index, url, sizeof url);
    snprintf(json, capacity, "{\"assets\":[{\"name\":\"weivel-index.json\",\"browser_download_url\":\"%s\"}]}", url);
}

/** A catalog listing the plugin p, whose one file is source.txt, and the file itself. */
static void a_catalog_with_one_plugin(void) {
    char bundles[1300];
    char source[1300];
    char url[4200];
    char text[5000];
    a_file_to_install();
    snprintf(bundles, sizeof bundles, "%s/bundles", place.state);
    under_base(source, sizeof source, "source.txt");
    file_url_of(source, url, sizeof url);
    snprintf(text, sizeof text,
             "{\"format\":1,\"items\":{\"p\":{\"kind\":\"plugin\",\"runtime\":\"native\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{"
             "\"files\":{\"p.js\":{\"url\":\"%s\",\"sha256\":\"" HELLO_SHA256 "\",\"size\":5}}}}}}}",
             url);
    write_text(place.base, "index.json", text);
    write_text(bundles, "spisor-host.json", "{\"run\":{\"old\":\"0.1.0\"},\"next\":{}}\n");
}

/**
 * Whether the ecosystem host's run is p alone, which the install records last: referencing p first adds it to the run
 * the base holds, and only the record replaces that run.
 */
static int recorded_the_run(void) {
    runtime_host_references references = runtime_read_host_references(place.state, "spisor-host");
    int recorded = cJSON_HasObjectItem(references.run, "p") && !cJSON_HasObjectItem(references.run, "old");
    runtime_free_host_references(&references);
    return recorded;
}

static int is_only_text(const char *const *texts, size_t count, const char *expected) {
    return count == 1 && strcmp(texts[0], expected) == 0;
}

/** Whether the run record no longer names the old version, read with the failing allocations suspended. */
static int replaced_the_old_run(void) {
    char path[1400];
    size_t length = 0;
    snprintf(path, sizeof path, "%s/bundles/spisor-host.json", place.state);
    spisor_memory_suspend_failures(1);
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_suspend_failures(0);
    int replaced = bytes != NULL && strstr((const char *)bytes, "\"old\"") == NULL;
    spisor_memory_release(bytes);
    return replaced;
}

/** Whether the answer installed and enabled p and found only spisor-host and runtime missing from the catalog. */
static int installed_the_plugin(const runtime_install_ecosystem_answer *answer) {
    return answer->installed_count == 1 && strcmp(answer->installed[0].id, "p") == 0 &&
           is_only_text(answer->enabled, answer->enabled_count, "p") && answer->unavailable_count == 2 &&
           answer->application_count == 0 && answer->javascript == NULL && replaced_the_old_run();
}

static spisor_status install_the_ecosystem(void) {
    const runtime_install_definition *install = runtime_install_definition_of();
    static const char *const defaults[1] = {"p"};
    char cache_place[1100];
    char index[1300];
    runtime_catalog_source source;
    runtime_install_ecosystem_argument argument = SPISOR_INIT(runtime_install_ecosystem_argument);
    runtime_install_ecosystem_answer answer = SPISOR_INIT(runtime_install_ecosystem_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    local_index_source(&source, index, sizeof index);
    under_base(cache_place, sizeof cache_place, "cache");
    argument.sources = &source;
    argument.source_count = 1;
    argument.portable = place.portable;
    argument.state = place.state;
    argument.cache = cache_place;
    argument.defaults = defaults;
    argument.default_count = 1;
    argument.platform = "linux-x64";
    spisor_status status = install->ecosystem(install->context, &argument, &answer, &refusal);
    if (status == SPISOR_OK && !installed_the_plugin(&answer)) {
        status = WRONG_ANSWER;
    }
    install->release_ecosystem_answer(install->context, &answer);
    install->release_refusal(install->context, &refusal);
    return status;
}

static void every_install_operation(void) {
    static const char repository[] =
        "{\"id\":5,\"name\":\"greeter\",\"full_name\":\"o/greeter\",\"default_branch\":\"main\"}";
    static char release_json[4400];
    release_json_of(release_json, sizeof release_json);
    http_route routes[] = {
        {"/repositories/5", 200, NULL, repository, sizeof repository - 1, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/o/greeter/HEAD/.weivel/also-at.json", 404, NULL, "", 0, HTTP_ROUTE_BODY_LENGTH, NULL, NULL, 0, 0},
        {"/repos/o/greeter/releases/latest", 200, NULL, release_json, strlen(release_json), HTTP_ROUTE_BODY_LENGTH,
         NULL, NULL, 0, 0},
    };
    http_fixture *server = http_fixture_start(routes, sizeof routes / sizeof routes[0]);
    CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    runtime_fetch_allow_plain_http(1);
    snprintf(by_id_api_base, sizeof by_id_api_base, "http://127.0.0.1:%u", (unsigned)http_fixture_port(server));
    failure_case by_id = {"install's by_id", install_by_id, a_release_to_install, NULL, installed_by_id, 0};
    every_failure_of(&by_id);
    runtime_fetch_allow_plain_http(0);
    spisor_memory_release(http_fixture_stop(server));
    failure_case ecosystem = {"install's ecosystem", install_the_ecosystem, a_catalog_with_one_plugin, NULL,
                              recorded_the_run, 1};
    every_failure_of(&ecosystem);
}

static void build_cache_and_catalog_values(void) {
    char source[1300];
    char url[4200];
    char text[5000];
    under_base(source, sizeof source, "source.txt");
    file_url_of(source, url, sizeof url);
    snprintf(text, sizeof text,
             "{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{\"p.js\":{\"url\":\"%s\","
             "\"sha256\":\"" HELLO_SHA256 "\",\"size\":5}}}}}",
             url);
    CHECK(value_of_text(text, &install_item));
    snprintf(text, sizeof text,
             "{\"kind\":\"plugin\",\"latest\":\"1.0.0\",\"versions\":{\"1.0.0\":{\"files\":{\"p.js\":{\"url\":\"%s\","
             "\"sha256\":\"" HELLO_SHA256 "\",\"size\":6}}}}}",
             url);
    CHECK(value_of_text(text, &short_item));
    CHECK(value_of_text(CATALOG_INDEX, &catalog_index));
    CHECK(value_of_text("{\"catalog\":{\"sources\":[{\"kind\":\"local\",\"path\":\"/x.json\"},"
                        "{\"kind\":\"https\",\"url\":\"https://e.test/c.json\"},{\"kind\":\"nope\"}]}}",
                        &catalog_settings));
}

static void release_cache_and_catalog_values(void) {
    spisor_value_release(&install_item);
    spisor_value_release(&short_item);
    spisor_value_release(&catalog_index);
    spisor_value_release(&catalog_settings);
}

/** The check must see a profile line nothing records, or it passes whatever a command leaves in a profile. */
static void an_unrecorded_profile_line_is_seen(void) {
    char line[1400];
    fresh_files();
    snprintf(line, sizeof line, "export PATH=\"%s/bin:$PATH\"\n", place.command_state);
    write_text(place.home, ".profile", line);
    CHECK(!every_change_is_recorded());
}

static void every_command(void) {
    an_unrecorded_profile_line_is_seen();
    every_failure_answers_out_of_memory("commands' remove", delete_a_command, deleted_nothing);
    every_command_failure("commands' request", request_a_command, NULL);
    every_command_failure("commands' add", add_a_command, NULL);
    every_command_failure("commands' rename", rename_a_command, renamed_to_dd);
    every_command_failure("commands' rename onto a held name", rename_onto_a_held_name, NULL);
}

/**
 * With the argument `commands` runs only the commands service, with `cache` only the cache and catalog services, and
 * with `install` only install's by_id and ecosystem, whose cases take most of the time, each as a test of its own. Any
 * other first argument runs the remaining services; a second narrows the cases to labels holding it, and a third and
 * fourth to the failures from first to last.
 */
int main(int argc, char **argv) {
    spisor_json_use_library_allocator();
    snprintf(place.base, sizeof place.base, RUNTIME_SCRATCH_PATH "/out-of-memory-%d", (int)CURRENT_PROCESS_ID);
    snprintf(place.home, sizeof place.home, "%s/home", place.base);
    snprintf(place.portable, sizeof place.portable, "%s/portable", place.base);
    snprintf(place.secrets_file, sizeof place.secrets_file, "%s/state/secrets.json", place.base);
    snprintf(place.config_directory, sizeof place.config_directory, "%s/config", place.home);
    snprintf(place.programs, sizeof place.programs, "%s/programs", place.base);
    snprintf(place.state, sizeof place.state, "%s/state", place.base);
    snprintf(place.command_state, sizeof place.command_state, "%s/command-state", place.base);
    CHECK(value_of_text("{\"cache\":\"c\"}", &names));
    CHECK(value_of_text("{\"defaults\":{\"a\":{\"d\":[1,\"two\"]}}}", &config));
    CHECK(value_of_text("{\"x\":[true,null,3]}", &value));
    CHECK(value_of_text("{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\",\"github:3\"]}",
                        &first_file));
    CHECK(value_of_text("{\"primary\":\"github:1\",\"ids\":[\"github:1\",\"github:2\"]}", &second_file));
    CHECK(value_of_text("[{\"id\":\"b\",\"label\":\"x\"},{\"id\":\"a\",\"n\":[1,2]}]", &plugin_rows));
    CHECK(value_of_text("{\"claude-host\":{\"kind\":\"host\",\"application\":{\"id\":\"claude\",\"label\":\"C\","
                        "\"binary\":\"probe\"}},\"opencode-host\":{\"kind\":\"host\",\"application\":{\"id\":"
                        "\"opencode\",\"binary\":\"absent\"}}}",
                        &catalog_items));
    CHECK(value_of_text("{\"format\":1,\"items\":{\"a\":{\"kind\":\"plugin\"},\"b\":{\"kind\":\"host\"}}}",
                        &whole_catalog));
    CHECK(value_of_text("{\"npm\":\"@vendor/tool\"}", &manifest_packages));
    build_cache_and_catalog_values();
    only_label = argc > 2 ? argv[2] : "";
    first_attempt = argc > 3 ? strtoull(argv[3], NULL, 10) : 1;
    last_attempt = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
    if (first_attempt == 0) {
        first_attempt = 1;
    }
    if (argc > 1 && strcmp(argv[1], "commands") == 0) {
        every_command();
    } else if (argc > 1 && strcmp(argv[1], "install") == 0) {
        every_install_operation();
    } else if (argc > 1 && strcmp(argv[1], "cache") == 0) {
        every_cache_and_catalog_operation();
    } else {
        every_service_but_commands();
    }
    spisor_value_release(&names);
    spisor_value_release(&config);
    spisor_value_release(&value);
    spisor_value_release(&first_file);
    spisor_value_release(&second_file);
    spisor_value_release(&plugin_rows);
    spisor_value_release(&catalog_items);
    spisor_value_release(&whole_catalog);
    spisor_value_release(&manifest_packages);
    release_cache_and_catalog_values();
    forget_test_path_value();
    CHECK(runtime_remove_tree(place.base) == SPISOR_OK);
    return check_summary("out_of_memory_test");
}
