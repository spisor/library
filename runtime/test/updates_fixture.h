#ifndef RUNTIME_TEST_UPDATES_FIXTURE_H
#define RUNTIME_TEST_UPDATES_FIXTURE_H

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "cache.h"
#include "catalog_call.h"
#include "catalog_url.h"
#include "check.h"
#include "directory_tree.h"
#include "hint_fixture.h"
#include "host_references.h"
#include "hosts.h"
#include "paths.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"
#include "updates.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <pthread.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define PLATFORM "linux-x64"
#define REINSTALL_FIX "run spisor-host install --catalog <path or url> again"

static char scratch[1024];
static char cache[1400];
static char state[1400];
static char catalog_folder[1400];
static cJSON *catalog_items;

static inline char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static inline void write_file(const char *path, const char *content) {
    char *parent = joined(path, "..");
    CHECK(spisor_make_directories(parent) == 1);
    free(parent);
    CHECK(spisor_replace_file(path, (const uint8_t *)content, strlen(content)) == 1);
}

static inline void write_in(const char *base, const char *rest, const char *content) {
    char *path = joined(base, rest);
    write_file(path, content);
    free(path);
}

static inline int exists_in(const char *base, const char *rest) {
    char *path = joined(base, rest);
    int exists = spisor_file_exists(path);
    free(path);
    return exists;
}

static inline char *text_in(const char *base, const char *rest) {
    char *path = joined(base, rest);
    size_t length = 0;
    char *text = (char *)spisor_read_file(path, &length);
    free(path);
    return text;
}

static inline void expect_text_in(const char *base, const char *rest, const char *expected, const char *label) {
    char *text = text_in(base, rest);
    check_text(text, expected, label);
    free(text);
}

static inline void expect_json(const cJSON *actual, const char *expected_json, const char *label) {
    cJSON *expected = parsed(expected_json);
    if (actual == NULL || !cJSON_Compare(actual, expected, 1)) {
        char *printed = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected_json, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
}

static inline void expect_json_in(const char *base, const char *rest, const char *expected_json, const char *label) {
    char *text = text_in(base, rest);
    cJSON *actual = text == NULL ? NULL : spisor_parse_json((const uint8_t *)text, strlen(text));
    expect_json(actual, expected_json, label);
    cJSON_Delete(actual);
    free(text);
}

static inline void expect_member_json(const cJSON *tree, const char *slashed, const char *expected_json, const char *label) {
    expect_json(value_at(tree, slashed), expected_json, label);
}

static inline void start_base(const char *label) {
    snprintf(scratch, sizeof scratch, "%s/updates-%d-%s", RUNTIME_SCRATCH_PATH, (int)current_process_id(), label);
    start_empty_directory(scratch);
    snprintf(cache, sizeof cache, "%s/cache", scratch);
    snprintf(state, sizeof state, "%s/state", scratch);
    snprintf(catalog_folder, sizeof catalog_folder, "%s/catalog", scratch);
    catalog_items = cJSON_CreateObject();
}

static inline void finish_base(void) {
    cJSON_Delete(catalog_items);
    catalog_items = NULL;
    CHECK(runtime_remove_tree(scratch) == 1);
}

static inline void sha256_of_text(const char *text, char out[RUNTIME_SHA256_HEX_SIZE]) {
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)text, strlen(text));
    runtime_sha256_finish_hex(&hash, out);
}

/** A FileRef to catalog/<id>/<version>/<name>, served holding served and listed as holding content. */
static inline cJSON *served_reference(const char *id, const char *version, const char *name, const char *content,
                               const char *served) {
    char rest[512];
    snprintf(rest, sizeof rest, "%s/%s/%s", id, version, name);
    char *path = joined(catalog_folder, rest);
    write_file(path, served);
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, path);
    free(path);
    char sha256[RUNTIME_SHA256_HEX_SIZE];
    sha256_of_text(content, sha256);
    cJSON *reference = cJSON_CreateObject();
    cJSON_AddStringToObject(reference, "url", url);
    free(url);
    cJSON_AddStringToObject(reference, "sha256", sha256);
    cJSON_AddNumberToObject(reference, "size", (double)strlen(content));
    return reference;
}

static inline cJSON *catalog_item(const char *id, const char *kind, const char *latest) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(catalog_items, id);
    if (item == NULL) {
        item = cJSON_AddObjectToObject(catalog_items, id);
        cJSON_AddStringToObject(item, "kind", kind);
        if (strcmp(kind, "host") == 0) {
            cJSON_AddStringToObject(item, "runtime", "native");
        }
        cJSON_AddStringToObject(item, "latest", latest);
        cJSON_AddObjectToObject(item, "versions");
    }
    cJSON_ReplaceItemInObjectCaseSensitive(item, "latest", cJSON_CreateString(latest));
    return item;
}

static inline void list_version(const char *id, const char *kind, const char *version, const char *latest,
                         const char *name, const char *served) {
    char content[256];
    snprintf(content, sizeof content, "%s@%s", id, version);
    cJSON *entry = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(catalog_item(id, kind, latest), "versions"),
                                           version);
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, name, served_reference(id, version, name, content, served == NULL ? content : served));
    if (strcmp(kind, "library") == 0) {
        cJSON *platform = cJSON_AddObjectToObject(cJSON_AddObjectToObject(entry, "platforms"), PLATFORM);
        cJSON_AddItemToObject(platform, "files", files);
    } else {
        cJSON_AddItemToObject(entry, "files", files);
    }
}

static inline void list_plugin(const char *id, const char *version, const char *latest) {
    list_version(id, "plugin", version, latest, "plugin.json", NULL);
}

/** A plugin version whose file is served with other bytes than the catalog lists, so it never installs. */
static inline void list_corrupt_plugin(const char *id, const char *version, const char *latest) {
    list_version(id, "plugin", version, latest, "plugin.json", "other bytes");
}

/** A native host version carrying its executable for PLATFORM, at the path boot.resolve runs. */
static inline void list_host(const char *id, const char *version, const char *latest) {
    char name[256];
    snprintf(name, sizeof name, PLATFORM "/%s", id);
    list_version(id, "host", version, latest, name, NULL);
}

static inline void list_library(const char *id, const char *version, const char *latest) {
    list_version(id, "library", version, latest, "lib.so", NULL);
}

static inline char *index_path(void) {
    return joined(catalog_folder, "index.json");
}

static inline void write_catalog(void) {
    cJSON *index = cJSON_CreateObject();
    cJSON_AddNumberToObject(index, "format", 1);
    cJSON_AddItemToObject(index, "items", cJSON_Duplicate(catalog_items, 1));
    char *text = cJSON_PrintUnformatted(index);
    cJSON_Delete(index);
    char *path = index_path();
    write_file(path, text);
    free(path);
    cJSON_free(text);
}

/** Settings reading the local catalog, with the members of extra_json added. */
static inline cJSON *settings_with(const char *extra_json) {
    cJSON *settings = parsed(extra_json);
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "kind", "local");
    char *path = index_path();
    cJSON_AddStringToObject(source, "path", path);
    free(path);
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source);
    cJSON_AddItemToObject(cJSON_AddObjectToObject(settings, "catalog"), "sources", sources);
    return settings;
}

static inline cJSON *places_json(void) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "state", state);
    cJSON_AddStringToObject(places, "cache", cache);
    return places;
}

static inline void install_listed(const char *id, const char *version) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "id", id);
    cJSON_AddItemToObject(argument, "item", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(catalog_items, id), 1));
    cJSON_AddStringToObject(argument, "version", version);
    cJSON_AddStringToObject(argument, "platform", PLATFORM);
    catalog_answer answer = call_catalog(runtime_cache_install, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

/** A version folder carrying an empty completion marker. */
static inline void mark_installed(const char *root, const char *id, const char *version) {
    char rest[512];
    if (strcmp(root, "libraries") == 0) {
        snprintf(rest, sizeof rest, "libraries/%s/%s/" PLATFORM "/.installed.json", id, version);
    } else {
        snprintf(rest, sizeof rest, "bundles/%s/%s/.installed.json", id, version);
    }
    write_in(cache, rest, "{\"files\":{}}");
}

static inline void record_references(const char *host, const char *json) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s.json", host);
    write_in(state, rest, json);
}

static inline void expect_references(const char *host, const char *expected_json, const char *label) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/%s.json", host);
    expect_json_in(state, rest, expected_json, label);
}

static inline void write_hint(const char *host, const char *json) {
    char rest[512];
    snprintf(rest, sizeof rest, "hosts/%s.json", host);
    write_in(cache, rest, json);
}

static inline void expect_hint(const char *host, const char *expected_json, const char *label) {
    char rest[512];
    snprintf(rest, sizeof rest, "hosts/%s.json", host);
    expect_json_in(cache, rest, expected_json, label);
}

/** Whether host's hint names the libraries at the versions expected_json pins, with its host, whatever their folders. */
static inline void expect_hinted_versions(const char *host, const char *expected_json, const char *label) {
    char rest[512];
    snprintf(rest, sizeof rest, "hosts/%s.json", host);
    char *text = text_in(cache, rest);
    cJSON *recorded = text == NULL ? NULL : spisor_parse_json((const uint8_t *)text, strlen(text));
    cJSON *versions = hinted_versions(recorded);
    expect_json(versions, expected_json, label);
    cJSON_Delete(versions);
    cJSON_Delete(recorded);
    free(text);
}

/** Registers host with the least a registration holds, unless it is registered or no plain file name. */
static inline void register_host(const char *host) {
    char rest[512];
    snprintf(rest, sizeof rest, "hosts/%s.json", host);
    if (runtime_is_plain_host_id(host) && !exists_in(state, rest)) {
        char registration[600];
        snprintf(registration, sizeof registration, "{\"id\":\"%s\"}", host);
        write_in(state, rest, registration);
    }
}

static inline catalog_answer apply_unregistered(const char *host, cJSON *settings, const char *extra_json) {
    cJSON *argument = parsed(extra_json);
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "hostId", host);
    cJSON_AddItemToObject(argument, "settings", settings);
    cJSON_AddStringToObject(argument, "platform", PLATFORM);
    return call_catalog(runtime_updates_apply, argument);
}

static inline catalog_answer apply_with(const char *host, cJSON *settings, const char *extra_json) {
    register_host(host);
    return apply_unregistered(host, settings, extra_json);
}

static inline catalog_answer check_unregistered(const char *host, cJSON *settings, const char *extra_json) {
    cJSON *argument = parsed(extra_json);
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "hostId", host);
    cJSON_AddStringToObject(argument, "now", "2026-11-04T12:00:00.000Z");
    cJSON_AddItemToObject(argument, "settings", settings);
    cJSON_AddStringToObject(argument, "platform", PLATFORM);
    return call_catalog(runtime_updates_check, argument);
}

static inline catalog_answer check_with(const char *host, cJSON *settings, const char *extra_json) {
    register_host(host);
    return check_unregistered(host, settings, extra_json);
}

static inline catalog_answer rollback_of(const char *host, const char *item) {
    register_host(host);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "hostId", host);
    cJSON_AddStringToObject(argument, "item", item);
    return call_catalog(runtime_updates_rollback, argument);
}

static inline catalog_answer recorded_of(const char *host) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddItemToObject(argument, "places", places_json());
    cJSON_AddStringToObject(argument, "hostId", host);
    return call_catalog(runtime_updates_recorded, argument);
}

static inline catalog_answer sweep_cache(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "state", state);
    return call_catalog(runtime_cache_sweep, argument);
}

static inline catalog_answer resolve_boot(const char *host, const char *arguments_json) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    if (host != NULL) {
        cJSON_AddStringToObject(argument, "hostId", host);
    }
    if (arguments_json != NULL) {
        cJSON_AddItemToObject(argument, "arguments", parsed(arguments_json));
    }
    cJSON_AddStringToObject(argument, "platform", PLATFORM);
    return call_catalog(runtime_boot_resolve, argument);
}

static inline int expect_success(catalog_answer answer, const char *label) {
    if (answer.status == SPISOR_STATUS_SUCCESS) {
        return 1;
    }
    fprintf(stderr, "%s: expected success, got status %d: %s\n", label, (int)answer.status,
            text_at(answer.tree, "message") == NULL ? "(none)" : text_at(answer.tree, "message"));
    check_failures++;
    return 0;
}

static inline void expect_fix(catalog_answer answer, const char *fix, const char *label) {
    check_text(text_at(answer.tree, "fix"), fix, label);
}

/** Whether answer names, as its executable, an existing file at cache/bundles/host/version/PLATFORM/host. */
static inline void expect_executable(catalog_answer answer, const char *host, const char *version, const char *label) {
    const char *executable = text_at(answer.tree, "executable");
    char expected[512];
    snprintf(expected, sizeof expected, "/bundles/%s/%s/" PLATFORM "/%s", host, version, host);
    char *normalized = NULL;
    if (executable != NULL) {
        normalized = malloc(strlen(executable) + 1);
        for (size_t index = 0; executable[index] != 0; index++) {
            normalized[index] = executable[index] == '\\' ? '/' : executable[index];
        }
        normalized[strlen(executable)] = 0;
    }
    size_t length = normalized == NULL ? 0 : strlen(normalized);
    int matched = normalized != NULL && length >= strlen(expected) &&
                  strcmp(normalized + length - strlen(expected), expected) == 0 && spisor_file_exists(executable);
    if (!matched) {
        fprintf(stderr, "%s: expected an existing executable ending %s, got %s\n", label, expected,
                executable == NULL ? "(none)" : executable);
        check_failures++;
    }
    free(normalized);
}

#endif
