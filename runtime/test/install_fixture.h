#ifndef RUNTIME_TEST_INSTALL_FIXTURE_H
#define RUNTIME_TEST_INSTALL_FIXTURE_H

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#include <wchar.h>
#include <windows.h>

#include "path_entry_windows_hooks.h"
#define fixture_process_id() _getpid()
#else
#include <unistd.h>
#define fixture_process_id() getpid()
#endif

#include "catalog_call.h"
#include "catalog_url.h"
#include "check.h"
#include "config.h"
#include "directory_tree.h"
#include "install_ecosystem.h"
#include "paths.h"
#include "platform_key.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

/** One test's places, all under its own scratch folder. */
typedef struct install_base {
    char root[1024];
    char *portable;
    char *state;
    char *cache;
    char *user;
    char *path;
    char *catalog;
    cJSON *items;
} install_base;

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
    uint8_t *bytes = spisor_read_file(path, &length);
    free(path);
    char *text = bytes == NULL ? NULL : malloc(length + 1);
    if (text != NULL) {
        memcpy(text, bytes, length);
        text[length] = 0;
    }
    free(bytes);
    return text;
}

static inline cJSON *json_in(const char *base, const char *rest) {
    char *text = text_in(base, rest);
    cJSON *tree = text == NULL ? NULL : spisor_parse_json((const uint8_t *)text, strlen(text));
    free(text);
    return tree;
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

static inline void start_install_base(install_base *base, const char *suite, const char *label) {
    snprintf(base->root, sizeof base->root, "%s/%s-%d-%s", RUNTIME_SCRATCH_PATH, suite, (int)fixture_process_id(), label);
    start_empty_directory(base->root);
    base->portable = joined(base->root, "portable");
    base->state = joined(base->root, "state");
    base->cache = joined(base->root, "cache");
    base->user = joined(base->root, "user");
    base->path = joined(base->root, "path");
    base->catalog = joined(base->root, "catalog");
    CHECK(spisor_make_directories(base->user) == 1 && spisor_make_directories(base->path) == 1);
    write_in(base->user, ".profile", "# profile\n");
    base->items = cJSON_CreateObject();
}

static inline void finish_install_base(install_base *base) {
    cJSON_Delete(base->items);
    CHECK(runtime_remove_tree(base->root) == 1);
    free(base->portable);
    free(base->state);
    free(base->cache);
    free(base->user);
    free(base->path);
    free(base->catalog);
}

static inline cJSON *places_of(const install_base *base) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "portable", base->portable);
    cJSON_AddStringToObject(places, "state", base->state);
    cJSON_AddStringToObject(places, "cache", base->cache);
    return places;
}

static inline cJSON *environment_with_path(const install_base *base, const char *path) {
    cJSON *environment = cJSON_CreateObject();
    cJSON_AddStringToObject(environment, "PATH", path);
    cJSON_AddStringToObject(environment, "HOME", base->user);
    cJSON_AddStringToObject(environment, "USERPROFILE", base->user);
    cJSON_AddStringToObject(environment, "SHELL", "/bin/sh");
    return environment;
}

static inline cJSON *environment_of(const install_base *base) {
    return environment_with_path(base, base->path);
}

static inline void sha256_of_text(const char *text, char out[RUNTIME_SHA256_HEX_SIZE]) {
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)text, strlen(text));
    runtime_sha256_finish_hex(&hash, out);
}

/** A FileRef to catalog/files/<id>/<version>/<name> holding content. */
static inline cJSON *served_reference(install_base *base, const char *id, const char *version, const char *name,
                                      const char *content) {
    char rest[512];
    snprintf(rest, sizeof rest, "files/%s/%s/%s", id, version, name);
    char *path = joined(base->catalog, rest);
    write_file(path, content);
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

/** The catalog item id, made with kind and runtime (NULL for none) the first time, latest moved to latest. */
static inline cJSON *catalog_item(install_base *base, const char *id, const char *kind, const char *runtime,
                                  const char *latest) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(base->items, id);
    if (item == NULL) {
        item = cJSON_AddObjectToObject(base->items, id);
        cJSON_AddStringToObject(item, "kind", kind);
        if (runtime != NULL) {
            cJSON_AddStringToObject(item, "runtime", runtime);
        }
        cJSON_AddStringToObject(item, "latest", latest);
        cJSON_AddObjectToObject(item, "versions");
    }
    cJSON_ReplaceItemInObjectCaseSensitive(item, "latest", cJSON_CreateString(latest));
    return item;
}

/** Lists version of id with one file name holding content, for every platform, or for this one when platform_only. */
static inline void list_file(install_base *base, cJSON *item, const char *id, const char *version, const char *name,
                             const char *content, int platform_only) {
    cJSON *entry = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(item, "versions"), version);
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, name, served_reference(base, id, version, name, content));
    if (platform_only) {
        cJSON *platform = cJSON_AddObjectToObject(cJSON_AddObjectToObject(entry, "platforms"), runtime_platform_key());
        cJSON_AddItemToObject(platform, "files", files);
    } else {
        cJSON_AddItemToObject(entry, "files", files);
    }
}

static inline void list_plugin(install_base *base, const char *id, const char *version, const char *runtime) {
    char content[256];
    snprintf(content, sizeof content, "{\"id\": \"%s\", \"version\": \"%s\"}", id, version);
    list_file(base, catalog_item(base, id, "plugin", runtime, version), id, version, "plugin.json", content, 0);
}

/** A plugin built for another platform only, which this machine cannot install. */
static inline void list_foreign_plugin(install_base *base, const char *id, const char *version) {
    cJSON *item = catalog_item(base, id, "plugin", NULL, version);
    cJSON *entry = cJSON_AddObjectToObject(cJSON_GetObjectItemCaseSensitive(item, "versions"), version);
    cJSON *platform = cJSON_AddObjectToObject(cJSON_AddObjectToObject(entry, "platforms"), "aix-ppc64");
    cJSON *files = cJSON_AddObjectToObject(platform, "files");
    cJSON_AddItemToObject(files, "plugin.json", served_reference(base, id, version, "plugin.json", "{}"));
}

static inline void list_library(install_base *base, const char *id, const char *version) {
    list_file(base, catalog_item(base, id, "library", NULL, version), id, version, "library.bin", id, 1);
}

static inline void list_host(install_base *base, const char *id, const char *version, const char *runtime,
                             const char *host_json) {
    list_file(base, catalog_item(base, id, "host", runtime, version), id, version, "host.json",
              host_json == NULL ? "{}" : host_json, 0);
}

/** The application JSON of id, found as binary, at home candidate ~/homes/<id>, under package mapping packages_json. */
/** An entry running the loader's hook through the boot stub, with one settings member the removal takes back. */
#define HOOK_ENTRY_JSON                                                                                              \
    "{\"runsOnJavascriptHost\": false, \"directories\": [\"{{entryRoot}}\", \"{{entryRoot}}/hooks\", \"{{home}}\"], " \
    "\"files\": [{\"path\": \"{{entryRoot}}/hooks/hooks.json\", \"json\": {\"hooks\": {\"SessionStart\": [{\"hooks\": " \
    "[{\"type\": \"command\", \"command\": \"{{boot}}\", \"args\": [\"{{bootedHost}}\", \"hook\", \"{{loaderId}}\", "  \
    "\"session-start\"], \"timeout\": 30}]}]}}}], "                                                                     \
    "\"settings\": [{\"file\": \"{{home}}/settings.json\", \"key\": [\"enabledPlugins\", \"spisor@spisor\"], \"value\": true}]}"

static inline cJSON *application_json(const char *id, const char *binary, const char *packages_json) {
    char text[1024];
    snprintf(text, sizeof text,
             "{\"id\": \"%s\", \"label\": \"%s\", \"binary\": \"%s\", \"home\": {\"candidates\": [\"~/homes/%s\"]}, "
             "\"packages\": %s}",
             id, id, binary, id, packages_json);
    return parsed(text);
}

static inline cJSON *application_with_hook_entry(const char *id, const char *binary, const char *packages_json) {
    cJSON *application = application_json(id, binary, packages_json);
    cJSON_AddItemToObject(application, "entry", parsed(HOOK_ENTRY_JSON));
    return application;
}

/** A loader host for application, its host.json the loader standard asking for commands, background hosts and required plugins. */
static inline void list_loader_requiring(install_base *base, const char *loader_id, const char *version, cJSON *application,
                                         const char *commands_json, int minimum, const char *background_json,
                                         const char *plugins_json) {
    cJSON *standard = cJSON_CreateObject();
    cJSON_AddItemToObject(standard, "application", cJSON_Duplicate(application, 1));
    cJSON *path_commands = cJSON_AddObjectToObject(standard, "pathCommands");
    cJSON_AddItemToObject(path_commands, "requested", parsed(commands_json));
    cJSON_AddNumberToObject(path_commands, "minimum", minimum);
    cJSON_AddItemToObject(standard, "background", parsed(background_json));
    cJSON_AddItemToObject(standard, "plugins", parsed(plugins_json));
    char *text = cJSON_PrintUnformatted(standard);
    cJSON_Delete(standard);
    cJSON *item = catalog_item(base, loader_id, "host", "native", version);
    if (cJSON_GetObjectItemCaseSensitive(item, "application") == NULL) {
        cJSON_AddItemToObject(item, "application", application);
    } else {
        cJSON_Delete(application);
    }
    list_file(base, item, loader_id, version, "host.json", text, 0);
    cJSON_free(text);
}

/** list_loader_requiring with no required plugins. */
static inline void list_loader(install_base *base, const char *loader_id, const char *version, cJSON *application,
                               const char *commands_json, int minimum, const char *background_json) {
    list_loader_requiring(base, loader_id, version, application, commands_json, minimum, background_json, "[]");
}

/** The foundation every ecosystem catalog carries: spisor-host and the runtime library. */
static inline void list_foundation(install_base *base) {
    list_host(base, "spisor-host", "1.0.0", "native", NULL);
    list_library(base, "runtime", "0.1.0");
}

static inline char *index_path_of(const install_base *base) {
    return joined(base->catalog, "catalog.json");
}

static inline void write_catalog_of(const install_base *base, const char *name) {
    cJSON *index = cJSON_CreateObject();
    cJSON_AddNumberToObject(index, "format", 1);
    cJSON_AddItemToObject(index, "items", cJSON_Duplicate(base->items, 1));
    char *text = cJSON_PrintUnformatted(index);
    cJSON_Delete(index);
    write_in(base->catalog, name, text);
    cJSON_free(text);
}

static inline void write_catalog(const install_base *base) {
    write_catalog_of(base, "catalog.json");
}

static inline cJSON *local_sources(const install_base *base) {
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "kind", "local");
    char *path = index_path_of(base);
    cJSON_AddStringToObject(source, "path", path);
    free(path);
    cJSON *sources = cJSON_CreateArray();
    cJSON_AddItemToArray(sources, source);
    return sources;
}

/** install.ecosystem over base's catalog with the members of extra_json added, environment taking PATH as given. */
static inline catalog_answer install_with(const install_base *base, const char *extra_json, cJSON *environment) {
    cJSON *argument = parsed(extra_json);
    if (!cJSON_HasObjectItem(argument, "sources")) {
        cJSON_AddItemToObject(argument, "sources", local_sources(base));
    }
    cJSON_AddItemToObject(argument, "places", places_of(base));
    cJSON_AddItemToObject(argument, "environment", environment == NULL ? environment_of(base) : environment);
    return call_catalog(runtime_install_ecosystem, argument);
}

static inline cJSON *expect_report(catalog_answer answer, const char *label) {
    if (answer.status != SPISOR_STATUS_SUCCESS) {
        char *printed = answer.tree == NULL ? NULL : cJSON_PrintUnformatted(answer.tree);
        fprintf(stderr, "%s: install refused: %s\n", label, printed == NULL ? "(nothing)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    return answer.tree;
}

/** The ids of the report's installed rows, sorted, as one JSON array. */
static inline cJSON *installed_ids(const cJSON *report) {
    cJSON *ids = cJSON_CreateArray();
    const cJSON *installed = cJSON_GetObjectItemCaseSensitive(report, "installed");
    int count = cJSON_GetArraySize(installed);
    const char *names[64];
    for (int index = 0; index < count && index < 64; index++) {
        names[index] = text_at(cJSON_GetArrayItem(installed, index), "id");
    }
    for (int outer = 0; outer < count && outer < 64; outer++) {
        for (int inner = outer + 1; inner < count && inner < 64; inner++) {
            if (strcmp(names[inner], names[outer]) < 0) {
                const char *swap = names[outer];
                names[outer] = names[inner];
                names[inner] = swap;
            }
        }
    }
    for (int index = 0; index < count && index < 64; index++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(names[index]));
    }
    return ids;
}

static inline void expect_installed_ids(const cJSON *report, const char *expected_json, const char *label) {
    cJSON *ids = installed_ids(report);
    expect_json(ids, expected_json, label);
    cJSON_Delete(ids);
}

/** The merged ecosystem settings of spisor-host's home and the portable place. */
static inline cJSON *ecosystem_settings(const install_base *base, int application, int portable) {
    char *home = joined(base->state, "home/spisor-host");
    char *no_home = joined(base->root, "no-home");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "home", application ? home : no_home);
    if (portable) {
        cJSON_AddStringToObject(argument, "portable", base->portable);
    }
    free(no_home);
    free(home);
    catalog_answer answer = call_catalog(runtime_settings_all, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *values = cJSON_DetachItemFromObjectCaseSensitive(answer.tree, "values");
    cJSON_Delete(answer.tree);
    return values;
}

static inline void set_portable(const install_base *base, const char *key, cJSON *value) {
    char *home = joined(base->state, "home/spisor-host");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "home", home);
    cJSON_AddStringToObject(argument, "portable", base->portable);
    cJSON_AddStringToObject(argument, "key", key);
    cJSON_AddItemToObject(argument, "value", value);
    cJSON_AddStringToObject(argument, "layer", "portable");
    free(home);
    catalog_answer answer = call_catalog(runtime_settings_set, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
}

#if defined(_WIN32)
#define ENVIRONMENT_KEY L"Environment"
#define READ_FLAGS (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND)

typedef struct registry_text {
    wchar_t text[8192];
    DWORD type;
    int present;
} registry_text;

static inline void read_registry_value(const wchar_t *name, registry_text *value) {
    DWORD size = sizeof value->text;
    value->type = 0;
    value->text[0] = L'\0';
    value->present = RegGetValueW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, name, READ_FLAGS, &value->type, value->text, &size) ==
                     ERROR_SUCCESS;
}

static inline void delete_test_path_value(void) {
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ENVIRONMENT_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, RUNTIME_PATH_ENTRY_TEST_VALUE);
        RegCloseKey(key);
    }
}
#endif

/** Keeps the real user Path as it was before the suite, which the commands' redirect hook must never touch. */
typedef struct user_path_guard {
#if defined(_WIN32)
    registry_text before;
#else
    int unused;
#endif
} user_path_guard;

static inline void guard_user_path(user_path_guard *guard) {
#if defined(_WIN32)
    read_registry_value(L"Path", &guard->before);
    delete_test_path_value();
#else
    guard->unused = 0;
#endif
}

static inline void expect_user_path_unchanged(const user_path_guard *guard) {
#if defined(_WIN32)
    delete_test_path_value();
    registry_text after;
    read_registry_value(L"Path", &after);
    CHECK(guard->before.present == after.present && guard->before.type == after.type &&
          wcscmp(guard->before.text, after.text) == 0);
#else
    (void)guard;
#endif
}

#endif
