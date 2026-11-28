/**
 * @file
 * A plugin written in C, built as a shared library on weivel/c's `host` module: each activation is appended to the
 * file C_FIXTURE_ACTIVATIONS_VARIABLE names, and every service its manifest provides answers the method `greet` with
 * "hello, <name>, from <import name>", the name its arguments' `name` gives, or "hello from <import name>" without one.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/c/host/entry.h>

#include "c_fixture_service.h"

#if defined(_WIN32)
#include <process.h>
#define C_FIXTURE_EXPORT __declspec(dllexport)
#define process_id() _getpid()
#else
#include <unistd.h>
#define C_FIXTURE_EXPORT __attribute__((visibility("default")))
#define process_id() getpid()
#endif

/** One activation of the plugin: its context, the object it registered, and the import name the object answers with. */
typedef struct activation {
    weivel_c_context *context;
    c_fixture_method_service service;
    char import_name[128];
} activation;

#define MOST_ACTIVATIONS 8

static activation *activations[MOST_ACTIVATIONS];

static int keep(activation *self) {
    for (size_t index = 0; index < MOST_ACTIVATIONS; index++) {
        if (activations[index] == NULL) {
            activations[index] = self;
            return 1;
        }
    }
    return 0;
}

static void record_activation(const char *import_name) {
    const char *path = getenv(C_FIXTURE_ACTIVATIONS_VARIABLE);
    FILE *file = path == NULL ? NULL : fopen(path, "ab");
    if (file != NULL) {
        fprintf(file, "%s %ld\n", import_name, (long)process_id());
        fclose(file);
    }
}

static const weivel_standard_value *text_member(const weivel_standard_value *record, const char *key) {
    if (record == NULL || record->kind != WEIVEL_STANDARD_VALUE_RECORD) {
        return NULL;
    }
    for (size_t index = 0; index < record->count; index++) {
        const weivel_standard_entry *entry = &record->entries[index];
        if (strlen(key) == entry->key_length && memcmp(entry->key, key, entry->key_length) == 0 &&
            entry->value.kind == WEIVEL_STANDARD_VALUE_TEXT) {
            return &entry->value;
        }
    }
    return NULL;
}

static int call(void *data, const char *method, const weivel_standard_value *arguments, char *result, size_t capacity) {
    const activation *self = data;
    if (strcmp(method, "greet") != 0) {
        snprintf(result, capacity, "%s offers no method %s", self->import_name, method);
        return 0;
    }
    const weivel_standard_value *name = text_member(arguments, "name");
    if (name == NULL) {
        snprintf(result, capacity, "hello from %s", self->import_name);
    } else {
        snprintf(result, capacity, "hello, %.*s, from %s", (int)name->length, name->text, self->import_name);
    }
    return 1;
}

static uint32_t activate(void *data, weivel_c_context *context, weivel_c_completion *completion, const char **reason) {
    (void)data;
    (void)completion;
    const weivel_manifest *manifest = NULL;
    if (weivel_c_context_manifest(context, &manifest) != WEIVEL_C_STATUS_OK || manifest == NULL) {
        *reason = "the plugin's manifest cannot be read from its context";
        return WEIVEL_C_ENTRY_FAILED;
    }
    activation *self = calloc(1, sizeof *self);
    if (self == NULL || !keep(self)) {
        free(self);
        *reason = "out of memory";
        return WEIVEL_C_ENTRY_FAILED;
    }
    self->context = context;
    self->service.size = (uint32_t)sizeof self->service;
    self->service.data = self;
    self->service.call = call;
    snprintf(self->import_name, sizeof self->import_name, "%s", manifest->import_name);
    for (uint32_t index = 0; index < manifest->services.provides.count; index++) {
        const char *id = manifest->services.provides.items[index].id;
        if (weivel_c_context_register_service(context, id, &self->service, NULL, NULL, NULL) != WEIVEL_C_STATUS_OK) {
            *reason = "a service the manifest provides could not be registered";
            return WEIVEL_C_ENTRY_FAILED;
        }
    }
    record_activation(self->import_name);
    return WEIVEL_C_ENTRY_DONE;
}

static uint32_t deactivate(void *data, weivel_c_context *context, weivel_c_completion *completion, const char **reason) {
    (void)data;
    (void)completion;
    (void)reason;
    for (size_t index = 0; index < MOST_ACTIVATIONS; index++) {
        if (activations[index] != NULL && activations[index]->context == context) {
            free(activations[index]);
            activations[index] = NULL;
        }
    }
    return WEIVEL_C_ENTRY_DONE;
}

C_FIXTURE_EXPORT weivel_c_status c_fixture_plugin_entry(weivel_c_entry *entry) {
    entry->activate = activate;
    entry->deactivate = deactivate;
    entry->data = NULL;
    return WEIVEL_C_STATUS_OK;
}
