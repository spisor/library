#include "install_entries.h"

#include <stdlib.h>
#include <string.h>

#include "boot_stub.h"
#include "cache_refusal.h"
#include "catalog_text.h"
#include "changes.h"
#include "entry_template.h"
#include "install_support.h"
#include "json_service.h"
#include "paths.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define ENTRY_ROOT_TEMPLATE "{{entryRoot}}"
#define ENTRY_FIX                                                                                            \
    "send {\"places\": {\"state\": \"<absolute directory>\", \"cache\": \"<absolute directory>\"}, \"loader\": " \
    "{\"id\", \"application\": {\"id\", \"label\", \"entry\"}, \"home\": \"<absolute directory>\"}}"

typedef struct entry_loader {
    const cJSON *places;
    const char *state;
    const char *cache;
    const char *id;
    const char *application;
    const char *label;
    const char *home;
    const cJSON *entry;
} entry_loader;

/** The paths the placeholders stand for, owned here, and the values that lend them out. */
typedef struct entry_context {
    char *entries;
    char *entry_root;
    char *boot;
    char *hint;
    char *bundles;
    runtime_entry_values values;
} entry_context;

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

static void finish_context(entry_context *context) {
    free(context->entries);
    free(context->entry_root);
    free(context->boot);
    free(context->hint);
    free(context->bundles);
}

static int start_context(entry_context *context, const entry_loader *loader) {
    const char *entries_segments[2] = {loader->state, "entries"};
    const char *root_segments[3] = {loader->state, "entries", loader->id};
    const char *hint_segments[3] = {loader->cache, "hosts", RUNTIME_JAVASCRIPT_HOST_ID ".json"};
    const char *bundle_segments[3] = {loader->cache, "bundles", RUNTIME_JAVASCRIPT_HOST_ID};
    context->entries = joined(entries_segments, 2);
    context->entry_root = joined(root_segments, 3);
    context->boot = runtime_boot_stub_path(loader->cache);
    context->hint = joined(hint_segments, 3);
    context->bundles = joined(bundle_segments, 3);
    runtime_entry_values values = {loader->home, context->entry_root, loader->id, context->boot,
                                   RUNTIME_BOOTED_HOST_ID, context->hint, context->bundles};
    context->values = values;
    return context->entries != NULL && context->entry_root != NULL && context->boot != NULL && context->hint != NULL &&
           context->bundles != NULL;
}

static int add_change(cJSON *changes, cJSON *change) {
    if (change == NULL || !cJSON_AddItemToArray(changes, change)) {
        cJSON_Delete(change);
        return 0;
    }
    return 1;
}

static int add_directory(cJSON *changes, char *path) {
    cJSON *change = path == NULL ? NULL : cJSON_CreateObject();
    int built = change != NULL && cJSON_AddStringToObject(change, "kind", "directory") != NULL &&
                cJSON_AddStringToObject(change, "path", path) != NULL;
    free(path);
    if (!built) {
        cJSON_Delete(change);
        return 0;
    }
    return add_change(changes, change);
}

static int add_file(cJSON *changes, char *path, char *content) {
    cJSON *change = path == NULL || content == NULL ? NULL : cJSON_CreateObject();
    int built = change != NULL && cJSON_AddStringToObject(change, "kind", "file") != NULL &&
                cJSON_AddStringToObject(change, "path", path) != NULL &&
                cJSON_AddStringToObject(change, "content", content) != NULL;
    free(path);
    free(content);
    if (!built) {
        cJSON_Delete(change);
        return 0;
    }
    return add_change(changes, change);
}

/** JSON.stringify(value, null, 2) and a newline; value is freed. */
static char *json_content(cJSON *value) {
    char *pretty = value == NULL ? NULL : spisor_json_stringify_pretty(value);
    cJSON_Delete(value);
    const char *parts[2] = {pretty, "\n"};
    char *content = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    free(pretty);
    return content;
}

/** The declared path with its placeholders filled, in the platform's own form. */
static char *expanded_path(const char *template_text, const runtime_entry_values *values) {
    char *expanded = runtime_entry_expand(template_text, values);
    const char *segments[1] = {expanded};
    char *path = expanded == NULL ? NULL : joined(segments, 1);
    free(expanded);
    return path;
}

static int expand_strings_in(cJSON *value, const runtime_entry_values *values) {
    if (cJSON_IsString(value)) {
        char *expanded = runtime_entry_expand(value->valuestring, values);
        char *set = expanded == NULL ? NULL : cJSON_SetValuestring(value, expanded);
        free(expanded);
        return set != NULL;
    }
    cJSON *child;
    cJSON_ArrayForEach(child, value) {
        if (!expand_strings_in(child, values)) {
            return 0;
        }
    }
    return 1;
}

/** A copy of template_value with every string value filled, a member's name left as it stands. */
static cJSON *expanded_json(const cJSON *template_value, const runtime_entry_values *values) {
    cJSON *copy = cJSON_Duplicate(template_value, 1);
    if (copy != NULL && !expand_strings_in(copy, values)) {
        cJSON_Delete(copy);
        return NULL;
    }
    return copy;
}

static int declares_entry_root(const cJSON *directories) {
    const cJSON *directory;
    cJSON_ArrayForEach(directory, directories) {
        if (strncmp(directory->valuestring, ENTRY_ROOT_TEMPLATE, strlen(ENTRY_ROOT_TEMPLATE)) == 0) {
            return 1;
        }
    }
    return 0;
}

/** @implNote The directory above entryRoot is this runtime's own layout, so an entry cannot declare it and removal must still take it. */
static int add_directories(const entry_context *context, const cJSON *directories, cJSON *changes) {
    if (declares_entry_root(directories) && !add_directory(changes, runtime_catalog_copy(context->entries))) {
        return 0;
    }
    const cJSON *directory;
    cJSON_ArrayForEach(directory, directories) {
        if (!add_directory(changes, expanded_path(directory->valuestring, &context->values))) {
            return 0;
        }
    }
    return 1;
}

static int add_files(const entry_context *context, const cJSON *files, cJSON *changes) {
    const cJSON *file;
    cJSON_ArrayForEach(file, files) {
        const cJSON *json = cJSON_GetObjectItemCaseSensitive(file, "json");
        const char *text = runtime_install_text(file, "text");
        char *content = json != NULL ? json_content(expanded_json(json, &context->values))
                                     : (text == NULL ? NULL : runtime_entry_expand(text, &context->values));
        if (!add_file(changes, expanded_path(runtime_install_text(file, "path"), &context->values), content)) {
            return 0;
        }
    }
    return 1;
}

static cJSON *setting_change(const entry_context *context, const cJSON *setting) {
    char *file = expanded_path(runtime_install_text(setting, "file"), &context->values);
    cJSON *key = expanded_json(cJSON_GetObjectItemCaseSensitive(setting, "key"), &context->values);
    cJSON *value = expanded_json(cJSON_GetObjectItemCaseSensitive(setting, "value"), &context->values);
    cJSON *change = file == NULL || key == NULL || value == NULL ? NULL : cJSON_CreateObject();
    int built = change != NULL && cJSON_AddStringToObject(change, "kind", "json-key") != NULL &&
                cJSON_AddStringToObject(change, "file", file) != NULL;
    free(file);
    if (!built) {
        cJSON_Delete(key);
        cJSON_Delete(value);
        cJSON_Delete(change);
        return NULL;
    }
    cJSON_AddItemToObject(change, "key", key);
    cJSON_AddItemToObject(change, "value", value);
    if (cJSON_AddBoolToObject(change, "create", 1) == NULL) {
        cJSON_Delete(change);
        return NULL;
    }
    return change;
}

static int add_settings(const entry_context *context, const cJSON *settings, cJSON *changes) {
    const cJSON *setting;
    cJSON_ArrayForEach(setting, settings) {
        if (!add_change(changes, setting_change(context, setting))) {
            return 0;
        }
    }
    return 1;
}

static int add_declared_changes(const entry_loader *loader, cJSON *changes) {
    entry_context context;
    int built = start_context(&context, loader) &&
                add_directories(&context, cJSON_GetObjectItemCaseSensitive(loader->entry, "directories"), changes) &&
                add_files(&context, cJSON_GetObjectItemCaseSensitive(loader->entry, "files"), changes) &&
                add_settings(&context, cJSON_GetObjectItemCaseSensitive(loader->entry, "settings"), changes);
    finish_context(&context);
    return built;
}

static cJSON *apply_call(const entry_loader *loader, const cJSON *change) {
    cJSON *call = runtime_install_places_call(loader->places);
    if (call == NULL || cJSON_AddStringToObject(call, "host", loader->id) == NULL ||
        !runtime_install_copy_into(call, "change", change)) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

static cJSON *applied(const entry_loader *loader, const cJSON *changes, cJSON **refusal) {
    cJSON *sequences = cJSON_CreateArray();
    for (const cJSON *change = sequences == NULL ? NULL : changes->child; change != NULL; change = change->next) {
        cJSON *answer = runtime_call_or_refuse(runtime_changes_apply_json, apply_call(loader, change), refusal);
        const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(answer, "sequence");
        int kept = answer != NULL && (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer, "recorded")) ||
                                      !cJSON_IsNumber(sequence) ||
                                      cJSON_AddItemToArray(sequences, cJSON_CreateNumber(sequence->valuedouble)));
        cJSON_Delete(answer);
        if (!kept) {
            cJSON_Delete(sequences);
            return NULL;
        }
    }
    return sequences;
}

static cJSON *written(cJSON *sequences, const cJSON *entry) {
    cJSON *answer = sequences == NULL ? NULL : cJSON_CreateObject();
    int on_javascript_host = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "runsOnJavascriptHost"));
    if (answer == NULL || cJSON_AddTrueToObject(answer, "written") == NULL ||
        (on_javascript_host && cJSON_AddTrueToObject(answer, "runsOnJavascriptHost") == NULL) ||
        !cJSON_AddItemToObject(answer, "sequences", sequences)) {
        cJSON_Delete(sequences);
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *not_written(const entry_loader *loader) {
    const char *message_parts[2] = {"this host writes no entry for the application ", loader->application};
    const char *fix_parts[3] = {"start ", loader->label, " through the command the loader requested"};
    char *message = runtime_cache_text(message_parts, 2);
    char *fix = runtime_cache_text(fix_parts, 3);
    cJSON *answer = message == NULL || fix == NULL ? NULL : cJSON_CreateObject();
    if (answer != NULL && (cJSON_AddFalseToObject(answer, "written") == NULL ||
                           cJSON_AddStringToObject(answer, "message", message) == NULL ||
                           cJSON_AddStringToObject(answer, "fix", fix) == NULL)) {
        cJSON_Delete(answer);
        answer = NULL;
    }
    free(fix);
    free(message);
    return answer;
}

static int read_loader(const cJSON *places, const cJSON *loader, entry_loader *read) {
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(loader, "application");
    read->places = places;
    read->state = runtime_install_text(places, "state");
    read->cache = runtime_install_text(places, "cache");
    read->id = runtime_install_text(loader, "id");
    read->application = runtime_install_text(application, "id");
    read->label = runtime_install_text(application, "label");
    read->home = runtime_install_text(loader, "home");
    read->entry = cJSON_GetObjectItemCaseSensitive(application, "entry");
    if (read->label == NULL) {
        read->label = read->application;
    }
    return read->state != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->state) && read->cache != NULL &&
           spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->cache) && read->id != NULL && read->id[0] != 0 &&
           read->application != NULL && read->home != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, read->home);
}

cJSON *runtime_install_entry(const cJSON *places, const cJSON *loader, cJSON **refusal) {
    entry_loader read;
    if (!read_loader(places, loader, &read)) {
        *refusal = runtime_refusal("the argument carries no usable places or loader", ENTRY_FIX);
        return NULL;
    }
    if (read.entry == NULL) {
        return not_written(&read);
    }
    cJSON *changes = cJSON_CreateArray();
    cJSON *answer = changes != NULL && add_declared_changes(&read, changes) ? written(applied(&read, changes, refusal), read.entry) : NULL;
    cJSON_Delete(changes);
    return answer;
}

static cJSON *entry_for(const cJSON *argument, cJSON **refusal) {
    return runtime_install_entry(cJSON_GetObjectItemCaseSensitive(argument, "places"),
                                 cJSON_GetObjectItemCaseSensitive(argument, "loader"), refusal);
}

RUNTIME_EXPORT int32_t runtime_install_entry_json(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, entry_for);
}
