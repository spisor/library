#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_context.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "config.h"
#include "json_service.h"
#include "places.h"
#include "registry.h"
#include "spisor/path.h"
#include "user_home.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#include "windows_process.h"
#else
#include <limits.h>
#include <unistd.h>
extern char **environ;
#endif

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

/** Whether environment already holds name, compared as Windows compares variable names, ignoring case. */
static int holds_name(const cJSON *environment, const char *name) {
    for (const cJSON *entry = environment->child; entry != NULL; entry = entry->next) {
        if (_stricmp(entry->string, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Adds one NAME=VALUE entry; an entry whose name starts with =, a drive's current directory, is left out. */
static int add_entry(cJSON *environment, const wchar_t *entry) {
    const wchar_t *equals = entry[0] == L'=' ? NULL : wcschr(entry, L'=');
    if (equals == NULL) {
        return 1;
    }
    char *name = narrow_of(entry, (int)(equals - entry));
    char *value = narrow_of(equals + 1, -1);
    int added = name != NULL && value != NULL && (holds_name(environment, name) || cJSON_AddStringToObject(environment, name, value) != NULL);
    free(value);
    free(name);
    return added;
}

cJSON *host_process_environment(void) {
    wchar_t *block = GetEnvironmentStringsW();
    cJSON *environment = block == NULL ? NULL : cJSON_CreateObject();
    for (const wchar_t *entry = environment == NULL ? NULL : block; entry != NULL && entry[0] != 0;
         entry += wcslen(entry) + 1) {
        if (!add_entry(environment, entry)) {
            cJSON_Delete(environment);
            environment = NULL;
            break;
        }
    }
    if (block != NULL) {
        FreeEnvironmentStringsW(block);
    }
    return environment;
}

double host_process_id(void) {
    return (double)_getpid();
}

#else

cJSON *host_process_environment(void) {
    cJSON *environment = cJSON_CreateObject();
    for (char **entry = environment == NULL ? NULL : environ; entry != NULL && *entry != NULL; entry++) {
        const char *equals = strchr(*entry, '=');
        if (equals == NULL || equals == *entry) {
            continue;
        }
        char *name = malloc((size_t)(equals - *entry) + 1);
        int added = name != NULL;
        if (added) {
            memcpy(name, *entry, (size_t)(equals - *entry));
            name[equals - *entry] = 0;
            added = cJSON_HasObjectItem(environment, name) || cJSON_AddStringToObject(environment, name, equals + 1) != NULL;
        }
        free(name);
        if (!added) {
            cJSON_Delete(environment);
            return NULL;
        }
    }
    return environment;
}

double host_process_id(void) {
    return (double)getpid();
}

#endif

static cJSON *places_call(const char *home, const cJSON *environment) {
    cJSON *call = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(environment, 1);
    if (call == NULL || cJSON_AddStringToObject(call, "home", home) == NULL || copy == NULL ||
        !cJSON_AddItemToObject(call, "environment", copy)) {
        cJSON_Delete(copy);
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

int host_open_context(host_context *context, cJSON **refusal) {
    memset(context, 0, sizeof *context);
    *refusal = NULL;
    context->environment = host_process_environment();
    const char *home = context->environment == NULL ? NULL : runtime_user_home_of(context->environment);
    if (context->environment != NULL && home == NULL) {
        *refusal = runtime_refusal(RUNTIME_NO_USER_HOME_MESSAGE, RUNTIME_NO_USER_HOME_FIX);
        return 0;
    }
    context->places = home == NULL ? NULL : host_call(runtime_places_resolve, places_call(home, context->environment), refusal);
    context->portable = host_text(context->places, "portable");
    context->state = host_text(context->places, "state");
    context->cache = host_text(context->places, "cache");
    return context->portable != NULL && context->state != NULL && context->cache != NULL;
}

void host_close_context(host_context *context) {
    cJSON_Delete(context->places);
    cJSON_Delete(context->environment);
    memset(context, 0, sizeof *context);
}

cJSON *host_places_argument(const host_context *context) {
    cJSON *argument = cJSON_CreateObject();
    cJSON *places = argument == NULL ? NULL : cJSON_AddObjectToObject(argument, "places");
    if (places == NULL || cJSON_AddStringToObject(places, "portable", context->portable) == NULL ||
        cJSON_AddStringToObject(places, "state", context->state) == NULL ||
        cJSON_AddStringToObject(places, "cache", context->cache) == NULL) {
        cJSON_Delete(argument);
        return NULL;
    }
    return argument;
}

cJSON *host_registration(const host_context *context, const char *id, cJSON **refusal) {
    cJSON *argument = host_places_argument(context);
    if (argument != NULL && cJSON_AddStringToObject(argument, "id", id) == NULL) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    cJSON *answer = host_call(runtime_registry_get, argument, refusal);
    cJSON *registration = cJSON_DetachItemFromObjectCaseSensitive(answer, "registration");
    cJSON_Delete(answer);
    if (registration == NULL && answer != NULL) {
        const char *parts[4] = {"the host ", id, " is not registered in ", context->state};
        *refusal = host_refusal_of(parts, 4, "run spisor-host install --catalog <path or url> again");
    }
    return registration;
}

cJSON *host_settings_of(const host_context *context, const char *home, cJSON **refusal) {
    cJSON *argument = cJSON_CreateObject();
    if (argument != NULL && (cJSON_AddStringToObject(argument, "home", home) == NULL ||
                             cJSON_AddStringToObject(argument, "portable", context->portable) == NULL)) {
        cJSON_Delete(argument);
        argument = NULL;
    }
    cJSON *answer = host_call(runtime_settings_all, argument, refusal);
    cJSON *values = cJSON_DetachItemFromObjectCaseSensitive(answer, "values");
    cJSON_Delete(answer);
    return values;
}

#if defined(_WIN32)

static char *utf8_of(const wchar_t *wide) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : malloc((size_t)bytes);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, bytes, NULL, NULL);
    }
    return narrow;
}

/** @implNote GetFullPathNameW on the UTF-16 path, since _fullpath works in the ANSI codepage. */
char *host_absolute_path(const char *path) {
    wchar_t *wide = runtime_windows_text(path);
    DWORD units = wide == NULL ? 0 : GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = units == 0 ? NULL : malloc(units * sizeof *full);
    char *absolute = full != NULL && GetFullPathNameW(wide, units, full, NULL) < units ? utf8_of(full) : NULL;
    free(full);
    free(wide);
    return absolute;
}

#else

char *host_absolute_path(const char *path) {
    char *absolute = NULL;
    if (spisor_path_is_absolute(0, path)) {
        absolute = malloc(strlen(path) + 1);
        if (absolute != NULL) {
            strcpy(absolute, path);
        }
    } else {
        char directory[PATH_MAX];
        const char *segments[2] = {getcwd(directory, sizeof directory), path};
        absolute = segments[0] == NULL ? NULL : spisor_path_join(0, segments, 2);
    }
    return absolute;
}

#endif

int host_equals_ignoring_case(const char *left, const char *right) {
    for (; *left != 0 && *right != 0; left++, right++) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
    }
    return *left == *right;
}
