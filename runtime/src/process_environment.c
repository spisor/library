#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_environment.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <wchar.h>
#include <windows.h>
#else
extern char **environ;
#endif

#if defined(_WIN32)

static char *narrow_of(const wchar_t *wide, int units) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, units, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : spisor_memory_allocate((size_t)bytes + 1);
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
    int added = name != NULL && value != NULL &&
                (holds_name(environment, name) || cJSON_AddStringToObject(environment, name, value) != NULL);
    spisor_memory_release(value);
    spisor_memory_release(name);
    return added;
}

cJSON *runtime_process_environment(void) {
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

#else

cJSON *runtime_process_environment(void) {
    cJSON *environment = cJSON_CreateObject();
    for (char **entry = environment == NULL ? NULL : environ; entry != NULL && *entry != NULL; entry++) {
        const char *equals = strchr(*entry, '=');
        if (equals == NULL || equals == *entry) {
            continue;
        }
        char *name = spisor_memory_allocate((size_t)(equals - *entry) + 1);
        int added = name != NULL;
        if (added) {
            memcpy(name, *entry, (size_t)(equals - *entry));
            name[equals - *entry] = 0;
            added = cJSON_HasObjectItem(environment, name) ||
                    cJSON_AddStringToObject(environment, name, equals + 1) != NULL;
        }
        spisor_memory_release(name);
        if (!added) {
            cJSON_Delete(environment);
            return NULL;
        }
    }
    return environment;
}

#endif
