#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "cache.h"
#include "catalog_time.h"
#include "file_times.h"
#include "hosts.h"
#include "install_ecosystem.h"
#include "json_service.h"
#include "places.h"
#include "spisor/json_write.h"
#include "updates.h"
#include "user_home.h"

#if defined(_WIN32)
#include <windows.h>
#else
extern char **environ;
#endif

/**
 * A host written in C over runtime's services and nothing else, answering install, status, update and
 * remove by calling the services spisor-host's commands call, so the proof compares two thin callers of
 * one implementation.
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

/** Writes the refusal as spisor-host does and answers 1. */
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

static cJSON *cache_call(const c_host *host) {
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "cache", host->cache);
    return call;
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
    cJSON *placed = runtime_call_or_refuse(runtime_boot_place, cache_call(host), &refusal);
    cJSON_Delete(placed);
    return placed == NULL ? refuse(refusal) : 0;
}

static int status(const c_host *host) {
    cJSON *refusal = NULL;
    cJSON *listed = runtime_call_or_refuse(runtime_hosts_list, places_call(host), &refusal);
    cJSON *installed = listed == NULL ? NULL : runtime_call_or_refuse(runtime_cache_installed, cache_call(host), &refusal);
    cJSON *found = installed == NULL ? NULL : runtime_call_or_refuse(runtime_cache_damaged, cache_call(host), &refusal);
    if (found == NULL) {
        cJSON_Delete(installed);
        cJSON_Delete(listed);
        return refuse(refusal);
    }
    cJSON_AddItemToObject(listed, "installed", installed);
    cJSON *damaged = cJSON_DetachItemFromObjectCaseSensitive(found, "damaged");
    cJSON_Delete(found);
    cJSON_AddItemToObject(listed, "damaged", damaged);
    print_json(listed);
    const cJSON *first = cJSON_GetArrayItem(damaged, 0);
    int code = first == NULL ? 0 : refuse(runtime_refusal(text_of(first, "detail"), text_of(first, "fix")));
    cJSON_Delete(listed);
    return code;
}

static int update(const c_host *host) {
    char now[RUNTIME_CATALOG_TIME_SIZE];
    runtime_catalog_format_time(runtime_now_milliseconds(), now);
    cJSON *call = places_call(host);
    cJSON_AddStringToObject(call, "now", now);
    cJSON *refusal = NULL;
    cJSON *summary = runtime_call_or_refuse(runtime_updates_run, call, &refusal);
    if (summary == NULL) {
        return refuse(refusal);
    }
    print_json(summary);
    int refused = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(summary, "refused")) > 0;
    cJSON_Delete(summary);
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
    cJSON *sweep = cache_call(host);
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
