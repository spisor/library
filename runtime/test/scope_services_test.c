#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "directory_tree.h"
#include "scope_fixture.h"
#include "spisor/json_allocation.h"

/**
 * @file
 * A plugin's `secrets`, `paths` and `log` through the scope runtime opens for it: secrets kept under that plugin's id
 * alone, directories the same on every call, and log entries handed to the writer and all written by the host's end.
 */

static scope_place place;

static int holds(const char *text, const char *part) {
    return text != NULL && strstr(text, part) != NULL;
}

static weivel_status open_scope(const char *manifest, weivel_standard_scope *scope) {
    const weivel_standard_scopes *scopes = scopes_definition();
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = scopes->open(scopes->context, manifest, scope, &refusal);
    if (status != WEIVEL_STATUS_OK) {
        fprintf(stderr, "the scope of %s did not open: %d \"%s\"\n", manifest, (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
    }
    scopes->release_refusal(scopes->context, &refusal);
    return status;
}

static void release_scope(weivel_standard_scope *scope) {
    scopes_definition()->release_scope(NULL, scope);
}

static weivel_status set_text(const weivel_standard_secrets *secrets, const char *name, const char *text,
                              weivel_standard_service_refusal *refusal) {
    return secrets->set(secrets->context, name, (const uint8_t *)text, strlen(text), refusal);
}

/** Whether `name` answers `text` exactly, or NONE when `text` is NULL. */
static int answers_secret(const weivel_standard_secrets *secrets, const char *name, const char *text) {
    weivel_standard_secret secret = WEIVEL_INIT(weivel_standard_secret);
    weivel_status status = secrets->get(secrets->context, name, &secret, NULL);
    int answered = text == NULL ? status == WEIVEL_STATUS_NONE && secret.storage == NULL
                                : status == WEIVEL_STATUS_OK && secret.length == strlen(text) &&
                                      memcmp(secret.bytes, text, secret.length) == 0;
    secrets->release_secret(secrets->context, &secret);
    CHECK(secret.storage == NULL && secret.bytes == NULL && secret.length == 0);
    return answered;
}

static void secrets_round_trip_for_the_plugin_alone(void) {
    weivel_standard_scope viewer = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_scope other = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_setting_answer answer = WEIVEL_INIT(weivel_standard_setting_answer);
    char *other_manifest = write_manifest(&place, "other.json", "{\"id\": \"other\"}");
    CHECK(open_scope(place.manifest, &viewer) == WEIVEL_STATUS_OK);
    CHECK(open_scope(other_manifest, &other) == WEIVEL_STATUS_OK);
    if (viewer.secrets != NULL && other.secrets != NULL) {
        CHECK(answers_secret(viewer.secrets, "api", NULL));
        CHECK(set_text(viewer.secrets, "api", "s3cr\xc3\xa9t", NULL) == WEIVEL_STATUS_OK);
        CHECK(answers_secret(viewer.secrets, "api", "s3cr\xc3\xa9t"));
        CHECK(answers_secret(other.secrets, "api", NULL));
        CHECK(set_text(viewer.secrets, "api", "", NULL) == WEIVEL_STATUS_OK);
        CHECK(answers_secret(viewer.secrets, "api", ""));
        CHECK(set_text(viewer.secrets, "token", "kept", NULL) == WEIVEL_STATUS_OK);
        CHECK(viewer.config->get(viewer.config->context, "token", &answer, NULL) == WEIVEL_STATUS_NONE);
        CHECK(viewer.secrets->remove(viewer.secrets->context, "api", NULL) == WEIVEL_STATUS_OK);
        CHECK(viewer.secrets->remove(viewer.secrets->context, "api", NULL) == WEIVEL_STATUS_OK);
        CHECK(answers_secret(viewer.secrets, "api", NULL));
        CHECK(answers_secret(viewer.secrets, "token", "kept"));
    }
    release_scope(&viewer);
    release_scope(&other);
    spisor_memory_release(other_manifest);
}

/** Requires the set of `bytes` to answer `expected` naming `detail`, and to store nothing. */
static void set_refuses(const weivel_standard_secrets *secrets, const char *bytes, size_t length,
                        weivel_status expected, const char *detail) {
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = secrets->set(secrets->context, "refused", (const uint8_t *)bytes, length, &refusal);
    if (status != expected || !holds(refusal.detail, detail) || refusal.fix == NULL) {
        fprintf(stderr, "a set naming \"%s\" answered %d \"%s\"\n", detail, (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
        check_failures++;
    }
    secrets->release_refusal(secrets->context, &refusal);
    CHECK(answers_secret(secrets, "refused", NULL));
}

static void secrets_refuse_what_runtime_cannot_keep(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_secret secret = WEIVEL_INIT(weivel_standard_secret);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    const weivel_standard_secrets *secrets;
    if (open_scope(place.manifest, &scope) != WEIVEL_STATUS_OK) {
        check_failures++;
        return;
    }
    secrets = scope.secrets;
    set_refuses(secrets, NULL, 3, WEIVEL_STATUS_MALFORMED, "a length without its bytes");
    set_refuses(secrets, "a\0b", 3, WEIVEL_STATUS_REFUSED, "a NUL byte");
    set_refuses(secrets, "\xff", 1, WEIVEL_STATUS_REFUSED, "not well-formed UTF-8");
    CHECK(secrets->set(secrets->context, NULL, (const uint8_t *)"x", 1, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(set_text(secrets, "", "x", &refusal) == WEIVEL_STATUS_REFUSED && holds(refusal.detail, "no usable name"));
    secrets->release_refusal(secrets->context, &refusal);
    CHECK(secrets->get(secrets->context, NULL, &secret, NULL) == WEIVEL_STATUS_MALFORMED);
    CHECK(secrets->remove(secrets->context, NULL, NULL) == WEIVEL_STATUS_MALFORMED);
    secret.size = (uint32_t)offsetof(weivel_standard_secret, storage);
    CHECK(secrets->get(secrets->context, "token", &secret, NULL) == WEIVEL_STATUS_MALFORMED);
    secret.size = (uint32_t)sizeof secret;
    secret.length = 1;
    CHECK(secrets->get(secrets->context, "token", &secret, NULL) == WEIVEL_STATUS_MALFORMED);
    secret.length = 0;
    refusal.storage = &refusal;
    CHECK(secrets->get(secrets->context, "token", &secret, &refusal) == WEIVEL_STATUS_MALFORMED);
    CHECK(set_text(secrets, "unset", "x", &refusal) == WEIVEL_STATUS_MALFORMED);
    CHECK(secrets->remove(secrets->context, "token", &refusal) == WEIVEL_STATUS_MALFORMED);
    CHECK(secret.storage == NULL && refusal.detail == NULL && answers_secret(secrets, "unset", NULL));
    CHECK(answers_secret(secrets, "token", "kept"));
    release_scope(&scope);
}

static void check_directory(const weivel_standard_paths *paths,
                            weivel_status (*operation)(void *context, const char **directory), const char *expected) {
    const char *first = NULL;
    const char *second = NULL;
    CHECK(operation(paths->context, &first) == WEIVEL_STATUS_OK);
    CHECK(operation(paths->context, &second) == WEIVEL_STATUS_OK);
    if (first == NULL || expected == NULL || strcmp(first, expected) != 0 || first != second) {
        fprintf(stderr, "a directory answered \"%s\" where \"%s\" was due, the same on every call\n",
                first == NULL ? "" : first, expected == NULL ? "" : expected);
        check_failures++;
    }
    CHECK(operation(paths->context, NULL) == WEIVEL_STATUS_MALFORMED);
}

/** A plugin's folder `id` under `base`/`within`, freed. */
static char *folder_under(const char *base, const char *within, const char *id) {
    char *parent = joined_path(base, within);
    char *folder = parent == NULL ? NULL : joined_path(parent, id);
    spisor_memory_release(parent);
    return folder;
}

/** Requires the scope of `manifest` to answer `id`'s own folders: its cache folder, and its own folder twice. */
static void check_plugin_directories(const char *manifest, const char *id, const char *own_place,
                                     const char *own_within) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    char *cache = folder_under(place.cache, "plugins", id);
    char *own = folder_under(own_place, own_within, id);
    if (open_scope(manifest, &scope) == WEIVEL_STATUS_OK) {
        check_directory(scope.paths, scope.paths->cache, cache);
        check_directory(scope.paths, scope.paths->config, own);
        check_directory(scope.paths, scope.paths->plugin, own);
    } else {
        check_failures++;
    }
    release_scope(&scope);
    spisor_memory_release(cache);
    spisor_memory_release(own);
}

/** Two plugins answer two sets of folders, none of them shared, with a portable layer and without one. */
static void paths_answer_the_plugins_own_folders(void) {
    char *other = write_manifest(&place, "other-paths.json", "{\"id\": \"other\"}");
    runtime_scope_host host = scope_host_of(&place);
    check_plugin_directories(place.manifest, "viewer", place.portable, "plugins");
    check_plugin_directories(other, "other", place.portable, "plugins");
    host.portable = NULL;
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
    check_plugin_directories(place.manifest, "viewer", place.home, "plugin");
    check_plugin_directories(other, "other", place.home, "plugin");
    host = scope_host_of(&place);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
    spisor_memory_release(other);
}

static weivel_status write_log(const weivel_standard_log *log, uint32_t level, const char *message,
                               const weivel_standard_value *fields, weivel_standard_service_refusal *refusal) {
    return log->write(log->context, level, message, fields, refusal);
}

/** The text of the one log file under the home, freed; NULL when there is not exactly one. */
static char *read_only_log(void) {
    runtime_directory_listing dates = {NULL, 0};
    runtime_directory_listing files = {NULL, 0};
    char *logs = joined_path(place.home, "logs");
    char *date = NULL;
    char *path = NULL;
    uint8_t *bytes = NULL;
    char *text = NULL;
    size_t length = 0;
    if (runtime_list_directory(logs, &dates) == SPISOR_OK && dates.count == 1) {
        date = joined_path(logs, dates.entries[0].name);
    }
    if (date != NULL && runtime_list_directory(date, &files) == SPISOR_OK && files.count == 1) {
        path = joined_path(date, files.entries[0].name);
    }
    if (path != NULL) {
        spisor_read_outcome outcome;
        bytes = spisor_read_file_reporting(path, &length, &outcome);
    }
    text = bytes == NULL ? NULL : spisor_memory_allocate(length + 1);
    if (text != NULL) {
        memcpy(text, bytes, length);
        text[length] = '\0';
    }
    spisor_memory_release(bytes);
    runtime_free_directory_listing(&dates);
    runtime_free_directory_listing(&files);
    spisor_memory_release(logs);
    spisor_memory_release(date);
    spisor_memory_release(path);
    return text;
}

static void log_writes_every_entry_by_the_hosts_end(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_entry entry = {"count", 5, {WEIVEL_STANDARD_VALUE_NUMBER, 0, 2.0, NULL, 0, NULL, NULL, 0}};
    weivel_standard_value fields = {WEIVEL_STANDARD_VALUE_RECORD, 0, 0.0, NULL, 0, NULL, &entry, 1};
    runtime_scope_host host = scope_host_of(&place);
    char message[64];
    char *text;
    if (open_scope(place.manifest, &scope) != WEIVEL_STATUS_OK) {
        check_failures++;
        return;
    }
    CHECK(write_log(scope.log, WEIVEL_STANDARD_LOG_INFO, "with fields", &fields, NULL) == WEIVEL_STATUS_OK);
    CHECK(write_log(scope.log, WEIVEL_STANDARD_LOG_TRACE, "never kept", NULL, NULL) == WEIVEL_STATUS_OK);
    for (int index = 0; index < 200; index++) {
        snprintf(message, sizeof message, "entry %d", index);
        CHECK(write_log(scope.log, WEIVEL_STANDARD_LOG_DEBUG, message, NULL, NULL) == WEIVEL_STATUS_OK);
    }
    release_scope(&scope);
    runtime_scopes_end();
    text = read_only_log();
    CHECK(holds(text, "[INFO] with fields {\"count\":2}\n") && holds(text, "[DEBUG] entry 0\n") &&
          holds(text, "[DEBUG] entry 199\n") && !holds(text, "never kept"));
    spisor_memory_release(text);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

/** Requires a write to answer `expected`, its refusal naming `detail`. */
static void write_refuses(const weivel_standard_log *log, uint32_t level, const char *message,
                          const weivel_standard_value *fields, weivel_status expected, const char *detail) {
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    weivel_status status = write_log(log, level, message, fields, &refusal);
    if (status != expected || !holds(refusal.detail, detail)) {
        fprintf(stderr, "a write naming \"%s\" answered %d \"%s\"\n", detail, (int)status,
                refusal.detail == NULL ? "" : refusal.detail);
        check_failures++;
    }
    log->release_refusal(log->context, &refusal);
}

static void log_refuses_what_services_h_does_not_allow(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_value list = {WEIVEL_STANDARD_VALUE_LIST, 0, 0.0, NULL, 0, NULL, NULL, 0};
    weivel_standard_entry bad = {"bad", 3, {WEIVEL_STANDARD_VALUE_TEXT, 0, 0.0, "\xff", 1, NULL, NULL, 0}};
    weivel_standard_value ill_formed = {WEIVEL_STANDARD_VALUE_RECORD, 0, 0.0, NULL, 0, NULL, &bad, 1};
    weivel_standard_service_refusal held = WEIVEL_INIT(weivel_standard_service_refusal);
    runtime_scope_host host = scope_host_of(&place);
    if (open_scope(place.manifest, &scope) != WEIVEL_STATUS_OK) {
        check_failures++;
        return;
    }
    write_refuses(scope.log, 0, "x", NULL, WEIVEL_STATUS_MALFORMED, "a level it does not know");
    write_refuses(scope.log, 6, "x", NULL, WEIVEL_STATUS_MALFORMED, "a level it does not know");
    write_refuses(scope.log, WEIVEL_STANDARD_LOG_WARN, NULL, NULL, WEIVEL_STATUS_MALFORMED, "no message");
    write_refuses(scope.log, WEIVEL_STANDARD_LOG_WARN, "x", &list, WEIVEL_STATUS_MALFORMED, "not a record");
    write_refuses(scope.log, WEIVEL_STANDARD_LOG_WARN, "x", &ill_formed, WEIVEL_STATUS_MALFORMED,
                  "not a well-formed value tree");
    held.storage = &held;
    CHECK(write_log(scope.log, WEIVEL_STANDARD_LOG_WARN, "x", NULL, &held) == WEIVEL_STATUS_MALFORMED);
    runtime_scopes_end();
    write_refuses(scope.log, WEIVEL_STANDARD_LOG_WARN, "after", NULL, WEIVEL_STATUS_REFUSED, "no host is started");
    release_scope(&scope);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
}

static void a_scope_refuses_an_id_no_cache_folder_can_carry(void) {
    weivel_standard_scope scope = WEIVEL_INIT(weivel_standard_scope);
    weivel_standard_service_refusal refusal = WEIVEL_INIT(weivel_standard_service_refusal);
    const weivel_standard_scopes *scopes = scopes_definition();
    runtime_scope_host host = scope_host_of(&place);
    char *manifest = write_manifest(&place, "slashed.json", "{\"id\": \"a:b\", \"config\": {\"name\": \"slashed\"}}");
    host.portable = NULL;
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
    CHECK(scopes->open(scopes->context, manifest, &scope, &refusal) == WEIVEL_STATUS_REFUSED);
    CHECK(holds(refusal.detail, "not a usable folder name") && scope.storage == NULL);
    scopes->release_refusal(scopes->context, &refusal);
    host = scope_host_of(&place);
    CHECK(runtime_scopes_begin(&host) == SPISOR_OK);
    spisor_memory_release(manifest);
}

int main(void) {
    spisor_json_use_library_allocator();
    start_scope_place(&place, "scope-services", "secrets");
    secrets_round_trip_for_the_plugin_alone();
    secrets_refuse_what_runtime_cannot_keep();
    paths_answer_the_plugins_own_folders();
    a_scope_refuses_an_id_no_cache_folder_can_carry();
    runtime_scopes_end();
    scope_place_free(&place);
    start_scope_place(&place, "scope-services", "log");
    log_writes_every_entry_by_the_hosts_end();
    log_refuses_what_services_h_does_not_allow();
    runtime_scopes_end();
    scope_place_free(&place);
    return check_summary("scope_services_test");
}
