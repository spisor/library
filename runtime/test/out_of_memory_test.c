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
#include "check.h"
#include "definition_fixture.h"
#include "json_allocation.h"
#include "runtime/changes.h"
#include "runtime/commands.h"
#include "runtime/config.h"
#include "runtime/detect.h"
#include "runtime/fetch.h"
#include "runtime/identity.h"
#include "runtime/lists.h"
#include "runtime/log.h"
#include "runtime/marketplace.h"
#include "runtime/paths.h"
#include "runtime/places.h"
#include "runtime/registry.h"
#include "runtime/secrets.h"
#include "runtime/settings.h"
#include "directory_tree.h"
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

static void write_text(const char *directory, const char *name, const char *text) {
    char path[1400];
    CHECK(spisor_make_directories(directory) == 1);
    snprintf(path, sizeof path, "%s/%s", directory, name);
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1);
}

#ifdef _WIN32
static void forget_test_path_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}

static int test_path_value_present(void) {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, L"Environment", RUNTIME_PATH_ENTRY_TEST_VALUE, RRF_RT_ANY, NULL, NULL,
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
static void fresh_files(void) {
    start_empty_directory(place.base);
    write_text(place.config_directory, "p.json", CHANGED_FILE);
    write_text(place.config_directory, "settings.json", "{\"logConsole\":true,\"ui\":{\"size\":2}}");
    write_text(place.portable, "settings.json", "{\"ui\":{\"theme\":\"dark\"}}");
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
    forget_test_path_value();
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

/** Whether a get left nothing at its destination. */
static int fetched_nothing(void) {
    char destination[1400];
    snprintf(destination, sizeof destination, "%s/fetched/settings.json", place.base);
    return !spisor_file_exists(destination);
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
    return !spisor_file_exists(record);
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
        uint8_t *bytes = spisor_read_file(profile, &length);
        int names_shims = bytes != NULL && strstr((const char *)bytes, shims) != NULL;
        free(bytes);
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
    int all = runtime_list_directory(shims, &listing);
    for (size_t index = 0; all && index < listing.count; index++) {
        all = recorded(registration, "file", "path", listing.entries[index].name);
    }
    runtime_free_directory_listing(&listing);
    all = all && (!test_path_value_present() || recorded(registration, "path-entry", NULL, NULL)) &&
          every_profile_line_is_recorded(registration);
    cJSON_Delete(registration);
    return all;
}

static int kept_zz_and_recorded_all(void) {
    return namer_holds_only("zz") && every_change_is_recorded();
}

/** Whether a rename took effect: namer's one active command is dd. */
static int renamed_to_dd(void) {
    return namer_holds_only("dd");
}


/**
 * One operation to fail allocation by allocation. `prepare` runs after the fresh files and before the first failure;
 * `left_alone` names what a failure must not have changed; `took_effect`, when it holds after a failure, means the
 * call completed and must answer OK; `sticky` fails every allocation after the first failure too.
 */
typedef struct failure_case {
    const char *label;
    operation run;
    void (*prepare)(void);
    untouched left_alone;
    untouched took_effect;
    int sticky;
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
static void every_failure_of(const failure_case *each) {
    const char *label = each->label;
    for (uint64_t attempt = 1; attempt < 100000; attempt++) {
        fresh_files();
        if (each->prepare != NULL) {
            each->prepare();
        }
        int64_t baseline = spisor_memory_live_blocks();
        if (each->sticky) {
            spisor_memory_fail_from(attempt);
        } else {
            spisor_memory_fail_at(attempt);
        }
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
            printf("%s: %llu allocations, each failure answered out of memory\n", label,
                   (unsigned long long)(attempt - 1));
            return;
        }
        if (!answered_as_required(each, attempt, status)) {
            check_failures++;
            return;
        }
    }
    fprintf(stderr, "%s: never ran whole\n", label);
    check_failures++;
}

static void every_failure_answers_out_of_memory(const char *label, operation run, untouched left_alone) {
    failure_case each = {label, run, NULL, left_alone, NULL, 0};
    every_failure_of(&each);
}

static void every_command_failure(const char *label, operation run, untouched took_effect, int sticky) {
    failure_case each = {label, run, add_zz, kept_zz_and_recorded_all, took_effect, sticky};
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
    every_command_failure("commands' request", request_a_command, NULL, 0);
    every_command_failure("commands' request, memory staying out", request_a_command, NULL, 1);
    every_command_failure("commands' add", add_a_command, NULL, 0);
    every_command_failure("commands' rename", rename_a_command, renamed_to_dd, 0);
    every_command_failure("commands' rename onto a held name", rename_onto_a_held_name, NULL, 0);
}

/** With the argument `commands` runs only the commands service, whose cases take most of the time, as a test of its own. */
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
    if (argc > 1 && strcmp(argv[1], "commands") == 0) {
        every_command();
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
    forget_test_path_value();
    CHECK(runtime_remove_tree(place.base) == 1);
    return check_summary("out_of_memory_test");
}
