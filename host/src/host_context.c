#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "host_context.h"
#include "allocation.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "places.h"
#include "process_environment.h"
#include "config.h"
#include "host_definition.h"
#include "registry.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#include "windows_process.h"
#else
#include <limits.h>
#include <unistd.h>
#endif

#if defined(_WIN32)

double host_process_id(void) {
    return (double)_getpid();
}

#else

double host_process_id(void) {
    return (double)getpid();
}

#endif

int host_open_context(host_context *context, cJSON **refusal) {
    runtime_places_answer empty = SPISOR_INIT(runtime_places_answer);
    memset(context, 0, sizeof *context);
    context->places = empty;
    *refusal = NULL;
    context->environment = runtime_process_environment();
    if (context->environment == NULL || !runtime_resolve_places_answer(context->environment, &context->places, refusal)) {
        return 0;
    }
    context->portable = context->places.portable;
    context->state = context->places.state;
    context->cache = context->places.cache;
    return 1;
}

void host_close_context(host_context *context) {
    const runtime_places_definition *places = runtime_places_definition_of();
    places->release_answer(places->context, &context->places);
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

/** Host id's registration as its record, NULL with *refusal set when there is none or it cannot be read. */
static cJSON *recorded_registration(const host_context *context, const char *id, cJSON **refusal) {
    const runtime_registry_definition *registry = runtime_registry_definition_of();
    runtime_registry_host_argument argument = SPISOR_INIT(runtime_registry_host_argument);
    runtime_registry_registration_answer answer = SPISOR_INIT(runtime_registry_registration_answer);
    spisor_refusal declined = SPISOR_INIT(spisor_refusal);
    argument.state = context->state;
    argument.id = id;
    spisor_status status = registry->get(registry->context, &argument, &answer, &declined);
    cJSON *registration = status == SPISOR_OK ? host_registration_json(answer.registration) : NULL;
    *refusal = status == SPISOR_OK || status == SPISOR_NONE ? NULL : host_refusal_json(&declined);
    registry->release_answer(registry->context, &answer);
    registry->release_refusal(registry->context, &declined);
    if (status == SPISOR_NONE) {
        runtime_registration_usable(NULL, id, context->state, refusal);
    }
    return registration;
}

cJSON *host_registration(const host_context *context, const char *id, cJSON **refusal) {
    cJSON *registration = recorded_registration(context, id, refusal);
    if (registration != NULL && !runtime_registration_usable(registration, id, context->state, refusal)) {
        cJSON_Delete(registration);
        registration = NULL;
    }
    return registration;
}

cJSON *host_settings_of(const host_context *context, const char *home, cJSON **refusal) {
    return runtime_ecosystem_settings_values(home, context->portable, refusal);
}

#if defined(_WIN32)

static char *utf8_of(const wchar_t *wide) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : spisor_memory_allocate((size_t)bytes);
    if (narrow != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, bytes, NULL, NULL);
    }
    return narrow;
}

/** @implNote GetFullPathNameW on the UTF-16 path, since _fullpath works in the ANSI codepage. */
char *host_absolute_path(const char *path) {
    wchar_t *wide = runtime_windows_text(path);
    DWORD units = wide == NULL ? 0 : GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = units == 0 ? NULL : spisor_memory_allocate(units * sizeof *full);
    char *absolute = full != NULL && GetFullPathNameW(wide, units, full, NULL) < units ? utf8_of(full) : NULL;
    spisor_memory_release(full);
    spisor_memory_release(wide);
    return absolute;
}

#else

char *host_absolute_path(const char *path) {
    char *absolute = NULL;
    if (spisor_path_is_absolute(0, path)) {
        absolute = spisor_memory_allocate(strlen(path) + 1);
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
