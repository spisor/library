#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "boot_stub.h"
#include "cache.h"
#include "catalog_time.h"
#include "config.h"
#include "file_times.h"
#include "hosts.h"
#include "install_ecosystem.h"
#include "json_service.h"
#include "paths.h"
#include "places.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"
#include "updates.h"
#include "user_home.h"

#if defined(_WIN32)
#include <windows.h>
#define BOOT_FILE "forebay-boot.exe"
#else
#include <sys/stat.h>
extern char **environ;
#define BOOT_FILE "forebay-boot"
#endif

#define PATH_METHOD "path"

/**
 * A host written in C over runtime's services and nothing else, answering install, status, update and
 * remove as forebay-host's commands do, so the proof can compare the trees the two leave.
 */
typedef struct c_host {
    cJSON *environment;
    cJSON *places;
    const char *state;
    const char *cache;
} c_host;

#if defined(_WIN32)

static char *narrow_of(const wchar_t *wide, int units) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, units, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : malloc((size_t)bytes + 1);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, wide, units, narrow, bytes, NULL, NULL);
        narrow[bytes] = 0;
    }
    return narrow;
}

static void add_variable(cJSON *environment, const wchar_t *entry) {
    const wchar_t *equals = entry[0] == L'=' ? NULL : wcschr(entry, L'=');
    char *name = equals == NULL ? NULL : narrow_of(entry, (int)(equals - entry));
    char *value = equals == NULL ? NULL : narrow_of(equals + 1, -1);
    if (name != NULL && value != NULL && !cJSON_HasObjectItem(environment, name)) {
        cJSON_AddStringToObject(environment, name, value);
    }
    free(value);
    free(name);
}

static cJSON *process_environment(void) {
    wchar_t *block = GetEnvironmentStringsW();
    cJSON *environment = cJSON_CreateObject();
    for (const wchar_t *entry = block; entry != NULL && entry[0] != 0; entry += wcslen(entry) + 1) {
        add_variable(environment, entry);
    }
    FreeEnvironmentStringsW(block);
    return environment;
}

#else

static cJSON *process_environment(void) {
    cJSON *environment = cJSON_CreateObject();
    for (char **entry = environ; *entry != NULL; entry++) {
        const char *equals = strchr(*entry, '=');
        if (equals == NULL || equals == *entry) {
            continue;
        }
        char *name = malloc((size_t)(equals - *entry) + 1);
        memcpy(name, *entry, (size_t)(equals - *entry));
        name[equals - *entry] = 0;
        if (!cJSON_HasObjectItem(environment, name)) {
            cJSON_AddStringToObject(environment, name, equals + 1);
        }
        free(name);
    }
    return environment;
}

#endif

static const char *text_of(const cJSON *object, const char *name) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static void print_json(const cJSON *value) {
    char *text = spisor_json_stringify_pretty(value);
    if (text != NULL) {
        fputs(text, stdout);
        fputs("\n", stdout);
    }
    free(text);
}

/** Writes the refusal as forebay-host does and answers 1. */
static int refuse(cJSON *refusal) {
    const char *message = text_of(refusal, "message");
    const char *fix = text_of(refusal, "fix");
    fprintf(stderr, "%s\nfix: %s\n", message == NULL ? "c-host ran out of memory" : message, fix == NULL ? "close other programs" : fix);
    cJSON_Delete(refusal);
    return 1;
}

static int open_host(c_host *host, cJSON **refusal) {
    memset(host, 0, sizeof *host);
    host->environment = process_environment();
    const char *home = runtime_user_home_of(host->environment);
    if (home == NULL) {
        *refusal = runtime_refusal(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
        return 0;
    }
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "home", home);
    cJSON_AddItemToObject(call, "environment", cJSON_Duplicate(host->environment, 1));
    host->places = runtime_call_or_refuse(runtime_places_resolve, call, refusal);
    host->state = text_of(host->places, "state");
    host->cache = text_of(host->places, "cache");
    return host->state != NULL && host->cache != NULL;
}

static void close_host(c_host *host) {
    cJSON_Delete(host->places);
    cJSON_Delete(host->environment);
}

static cJSON *places_call(const c_host *host) {
    cJSON *call = cJSON_CreateObject();
    cJSON *places = cJSON_AddObjectToObject(call, "places");
    cJSON_AddStringToObject(places, "portable", text_of(host->places, "portable"));
    cJSON_AddStringToObject(places, "state", host->state);
    cJSON_AddStringToObject(places, "cache", host->cache);
    return call;
}

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

static int copy_runnable(const char *source, const char *destination) {
    const char *parent_segments[2] = {destination, ".."};
    char *parent = joined(parent_segments, 2);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(source, &length);
    int copied = bytes != NULL && spisor_make_directories(parent) == 1 && spisor_replace_file(destination, bytes, length) == 1;
#if !defined(_WIN32)
    copied = copied && chmod(destination, 0755) == 0;
#endif
    free(bytes);
    free(parent);
    return copied;
}

/** Puts the forebay-boot the hinted forebay-host build carries at the boot stub's path, when none is there yet. */
static int place_boot_stub(const c_host *host) {
    char *stub = runtime_boot_stub_path(host->cache);
    if (spisor_file_exists(stub)) {
        free(stub);
        return 1;
    }
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "cache", host->cache);
    cJSON_AddStringToObject(call, "hostId", RUNTIME_BOOTED_HOST_ID);
    cJSON *refusal = NULL;
    cJSON *resolved = runtime_call_or_refuse(runtime_boot_resolve, call, &refusal);
    cJSON_Delete(refusal);
    const char *segments[3] = {text_of(resolved, "directory"), runtime_platform_key(), BOOT_FILE};
    char *carried = segments[0] == NULL ? NULL : joined(segments, 3);
    int placed = carried == NULL || !spisor_file_exists(carried) || copy_runnable(carried, stub);
    free(carried);
    cJSON_Delete(resolved);
    free(stub);
    return placed;
}

static cJSON *ecosystem_call(const c_host *host, const char *catalog, char **plugins, int plugin_count) {
    cJSON *call = places_call(host);
    cJSON *source = cJSON_CreateObject();
    cJSON_AddStringToObject(source, "kind", "local");
    cJSON_AddStringToObject(source, "path", catalog);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(call, "sources"), source);
    cJSON_AddItemToObject(call, "environment", cJSON_Duplicate(host->environment, 1));
    cJSON_AddObjectToObject(call, "choices");
    cJSON *requested = cJSON_AddArrayToObject(call, "requested");
    for (int index = 0; index < plugin_count; index++) {
        cJSON_AddItemToArray(requested, cJSON_CreateString(plugins[index]));
    }
    return call;
}

static int install(const c_host *host, const char *catalog, char **plugins, int plugin_count) {
    cJSON *refusal = NULL;
    cJSON *report = runtime_call_or_refuse(runtime_install_ecosystem, ecosystem_call(host, catalog, plugins, plugin_count), &refusal);
    if (report == NULL) {
        return refuse(refusal);
    }
    print_json(report);
    cJSON_Delete(report);
    return place_boot_stub(host) ? 0 : refuse(runtime_refusal("forebay-boot could not be placed", "make the cache writable"));
}

/** Adds {detail, fix, file?, version, id} to damaged for each version under root that cache.check, which hashes, finds damaged. */
static void add_damaged(const c_host *host, const cJSON *versions_of, const char *root, int per_platform, cJSON *damaged) {
    const cJSON *versions;
    cJSON_ArrayForEach(versions, versions_of) {
        const cJSON *version;
        cJSON_ArrayForEach(version, versions) {
            const char *segments[5] = {host->cache, root, versions->string, version->valuestring, runtime_platform_key()};
            char *directory = joined(segments, per_platform ? 5 : 4);
            cJSON *call = cJSON_CreateObject();
            cJSON_AddStringToObject(call, "directory", directory);
            free(directory);
            cJSON *refusal = NULL;
            cJSON *check = runtime_call_or_refuse(runtime_cache_check, call, &refusal);
            check = check == NULL ? refusal : check;
            const char *kind = text_of(check, "kind");
            if (kind != NULL && strcmp(kind, "intact") == 0) {
                cJSON_Delete(check);
                continue;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(check, "kind");
            cJSON_AddStringToObject(check, "version", version->valuestring);
            cJSON_AddStringToObject(check, "id", versions->string);
            cJSON_AddItemToArray(damaged, check);
        }
    }
}

static int status(const c_host *host) {
    cJSON *refusal = NULL;
    cJSON *listed = runtime_call_or_refuse(runtime_hosts_list, places_call(host), &refusal);
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "cache", host->cache);
    cJSON *installed = listed == NULL ? NULL : runtime_call_or_refuse(runtime_cache_installed, call, &refusal);
    if (listed == NULL || installed == NULL) {
        cJSON_Delete(listed);
        return refuse(refusal);
    }
    cJSON_AddItemToObject(listed, "installed", installed);
    cJSON *damaged = cJSON_AddArrayToObject(listed, "damaged");
    add_damaged(host, cJSON_GetObjectItemCaseSensitive(installed, "bundles"), "bundles", 0, damaged);
    add_damaged(host, cJSON_GetObjectItemCaseSensitive(installed, "libraries"), "libraries", 1, damaged);
    print_json(listed);
    const cJSON *first = cJSON_GetArrayItem(damaged, 0);
    int code = first == NULL ? 0 : refuse(runtime_refusal(text_of(first, "detail"), text_of(first, "fix")));
    cJSON_Delete(listed);
    return code;
}

/** Every registration hosts.list answers, forebay-host first, so its catalog check serves the rest. */
static cJSON *registrations_of(const c_host *host) {
    cJSON *refusal = NULL;
    cJSON *listed = runtime_call_or_refuse(runtime_hosts_list, places_call(host), &refusal);
    cJSON_Delete(refusal);
    cJSON *registrations = cJSON_CreateArray();
    for (int pass = 0; pass < 2; pass++) {
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(listed, "hosts")) {
            const cJSON *registration = cJSON_GetObjectItemCaseSensitive(row, "registration");
            const char *id = text_of(registration, "id");
            if (id != NULL && (strcmp(id, RUNTIME_BOOTED_HOST_ID) == 0) == (pass == 0)) {
                cJSON_AddItemToArray(registrations, cJSON_Duplicate(registration, 1));
            }
        }
    }
    cJSON_Delete(listed);
    return registrations;
}

static cJSON *settings_of(const c_host *host, const char *home) {
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "home", home);
    cJSON_AddStringToObject(call, "portable", text_of(host->places, "portable"));
    cJSON *refusal = NULL;
    cJSON *answer = runtime_call_or_refuse(runtime_settings_all, call, &refusal);
    cJSON_Delete(refusal);
    cJSON *values = cJSON_DetachItemFromObjectCaseSensitive(answer, "values");
    cJSON_Delete(answer);
    return values == NULL ? cJSON_CreateObject() : values;
}

/** The registration's own settings with forebay-host's catalog sources, which install records in its layer alone. */
static cJSON *check_call(const c_host *host, const cJSON *registration, const cJSON *ecosystem) {
    cJSON *settings = settings_of(host, text_of(registration, "home"));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "catalog");
    const cJSON *catalog = cJSON_GetObjectItemCaseSensitive(ecosystem, "catalog");
    if (catalog != NULL) {
        cJSON_AddItemToObject(settings, "catalog", cJSON_Duplicate(catalog, 1));
    }
    char now[RUNTIME_CATALOG_TIME_SIZE];
    runtime_catalog_format_time(runtime_now_milliseconds(), now);
    cJSON *call = places_call(host);
    cJSON_AddStringToObject(call, "hostId", text_of(registration, "id"));
    cJSON_AddStringToObject(call, "now", now);
    cJSON_AddItemToObject(call, "settings", settings);
    return call;
}

static int has_method_application(const cJSON *registration) {
    const char *method = text_of(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(registration, "application"), "provenance"), "method");
    return method != NULL && strcmp(method, PATH_METHOD) != 0;
}

static void record(cJSON *answers, cJSON *refused, const char *id, cJSON *answer, cJSON *refusal) {
    if (answer != NULL) {
        cJSON_AddItemToObject(answers, id, answer);
    } else if (refusal != NULL) {
        cJSON_AddStringToObject(refusal, "id", id);
        cJSON_AddItemToArray(refused, refusal);
    }
}

static void update_one(const c_host *host, const cJSON *registration, const cJSON *ecosystem, cJSON *summary) {
    const char *id = text_of(registration, "id");
    cJSON *refusal = NULL;
    cJSON *checked = runtime_call_or_refuse(runtime_updates_check, check_call(host, registration, ecosystem), &refusal);
    record(cJSON_GetObjectItemCaseSensitive(summary, "checks"), cJSON_GetObjectItemCaseSensitive(summary, "refused"), id, checked,
           refusal);
    if (checked != NULL && has_method_application(registration)) {
        cJSON *call = places_call(host);
        cJSON_AddStringToObject(call, "loader", id);
        cJSON_AddStringToObject(call, "operation", "update");
        refusal = NULL;
        cJSON *updated = runtime_call_or_refuse(runtime_install_application, call, &refusal);
        record(cJSON_GetObjectItemCaseSensitive(summary, "applications"), cJSON_GetObjectItemCaseSensitive(summary, "refused"), id,
               updated, refusal);
    }
}

static int update(const c_host *host) {
    cJSON *registrations = registrations_of(host);
    const cJSON *first = cJSON_GetArrayItem(registrations, 0);
    int ecosystem_first = first != NULL && strcmp(text_of(first, "id"), RUNTIME_BOOTED_HOST_ID) == 0;
    cJSON *ecosystem = ecosystem_first ? settings_of(host, text_of(first, "home")) : cJSON_CreateObject();
    cJSON *summary = cJSON_CreateObject();
    cJSON_AddObjectToObject(summary, "checks");
    cJSON_AddObjectToObject(summary, "applications");
    cJSON_AddArrayToObject(summary, "refused");
    const cJSON *registration;
    cJSON_ArrayForEach(registration, registrations) {
        update_one(host, registration, ecosystem, summary);
    }
    print_json(summary);
    int refused = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(summary, "refused")) > 0;
    cJSON_Delete(summary);
    cJSON_Delete(ecosystem);
    cJSON_Delete(registrations);
    return refused ? refuse(runtime_refusal("not everything could be updated", "see refused above")) : 0;
}

static int remove_host(const c_host *host, const char *id) {
    cJSON *call = places_call(host);
    cJSON_AddStringToObject(call, "id", id);
    cJSON *refusal = NULL;
    cJSON *removed = runtime_call_or_refuse(runtime_hosts_remove, call, &refusal);
    if (removed == NULL) {
        return refuse(refusal);
    }
    cJSON *sweep = cJSON_CreateObject();
    cJSON_AddStringToObject(sweep, "cache", host->cache);
    cJSON_AddStringToObject(sweep, "state", host->state);
    cJSON *swept = runtime_call_or_refuse(runtime_cache_sweep, sweep, &refusal);
    if (swept == NULL) {
        cJSON_Delete(removed);
        return refuse(refusal);
    }
    cJSON *summary = cJSON_CreateObject();
    cJSON_AddItemToObject(summary, "removed", removed);
    cJSON_AddItemToObject(summary, "swept", swept);
    print_json(summary);
    cJSON_Delete(summary);
    return 0;
}

static int answer(const c_host *host, int count, char **arguments) {
    if (count >= 4 && strcmp(arguments[1], "install") == 0) {
        return install(host, arguments[2], arguments + 3, count - 3);
    }
    if (count == 2 && strcmp(arguments[1], "status") == 0) {
        return status(host);
    }
    if (count == 2 && strcmp(arguments[1], "update") == 0) {
        return update(host);
    }
    if (count == 3 && strcmp(arguments[1], "remove") == 0) {
        return remove_host(host, arguments[2]);
    }
    fputs("usage: c-host install <catalog> <plugin>... | status | update | remove <id>\n", stderr);
    return 2;
}

static int run(int count, char **arguments) {
    c_host host;
    cJSON *refusal = NULL;
    int code = open_host(&host, &refusal) ? answer(&host, count, arguments) : refuse(refusal);
    close_host(&host);
    return code;
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = calloc((size_t)count + 1, sizeof *arguments);
    for (int index = 0; index < count; index++) {
        arguments[index] = narrow_of(wide[index], -1);
    }
    int code = run(count, arguments);
    for (int index = 0; index < count; index++) {
        free(arguments[index]);
    }
    free(arguments);
    return code;
}

#else

int main(int count, char **arguments) {
    return run(count, arguments);
}

#endif
