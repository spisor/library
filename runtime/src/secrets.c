#include "secrets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "secret_backend.h"
#include "secret_file.h"
#include "spisor/json_kind.h"
#include "spisor/path.h"

typedef struct secret_request {
    const char *secrets_file;
    int force_file_store;
    const cJSON *argument;
} secret_request;

typedef int32_t (*secret_operation)(const secret_request *request, uint8_t **out, size_t *out_len);

typedef struct move_result {
    runtime_secret_file_answer file;
    int moved;
} move_result;

void runtime_free_secret_names(runtime_secret_names *names) {
    for (size_t index = 0; names->names != NULL && index < names->count; index++) {
        free(names->names[index]);
    }
    free(names->names);
    names->names = NULL;
    names->count = 0;
}

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t refuse_file(const char *file_path, runtime_secret_file_answer answer, uint8_t **out, size_t *out_len) {
    char fix[2048];
    if (answer == RUNTIME_SECRET_FILE_OUT_OF_MEMORY) {
        return out_of_memory(out, out_len);
    }
    if (answer == RUNTIME_SECRET_FILE_UNREADABLE) {
        snprintf(fix, sizeof fix, "repair %s so it holds one JSON object of strings, or remove it; it was left unchanged",
                 file_path);
        return spisor_fail("the secrets file is not a readable JSON object", fix, out, out_len);
    }
    if (answer == RUNTIME_SECRET_FILE_LINKED) {
        snprintf(fix, sizeof fix,
                 "replace the link at %s with a regular file, or set SPISOR_SECRETS to a private directory to keep "
                 "the secrets file elsewhere; it was left unchanged",
                 file_path);
        return spisor_fail("the secrets file is a symbolic link, and a secret is never written through one", fix,
                           out, out_len);
    }
    snprintf(fix, sizeof fix, "check that %s and its directory can be written", file_path);
    return spisor_fail("the secrets file could not be written", fix, out, out_len);
}

static int32_t refuse_store(const char *action, runtime_secret_answer answer, uint8_t **out, size_t *out_len) {
    char message[256];
    snprintf(message, sizeof message, "the OS credential store %s while asked to %s the secret",
             answer == RUNTIME_SECRET_DECLINED ? "declined" : "failed", action);
    return spisor_fail(message, "unlock the credential store or allow this program to use it, then try again", out,
                       out_len);
}

static int32_t answer_with(cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static int32_t answer_string(const char *member, const char *value, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && value != NULL && cJSON_AddStringToObject(result, member, value) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return answer_with(result, out, out_len);
}

static int32_t answer_moved(int moved, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && cJSON_AddNumberToObject(result, "moved", moved) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return answer_with(result, out, out_len);
}

static const char *string_member(const cJSON *argument, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(argument, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int is_usable_plugin(const char *plugin) {
    return plugin != NULL && plugin[0] != '\0' && strchr(plugin, '/') == NULL;
}

static char *entry_name(const char *plugin, const char *name) {
    size_t size = sizeof("spisor//") + strlen(plugin) + strlen(name);
    char *entry = malloc(size);
    if (entry != NULL) {
        snprintf(entry, size, "spisor/%s/%s", plugin, name);
    }
    return entry;
}

static int32_t refuse_plugin(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable plugin",
                       "send {\"plugin\": \"<manifest id>\"}, a non-empty id without a slash", out, out_len);
}

static int32_t refuse_name(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable name", "send {\"name\": \"<secret name>\"}, a non-empty string",
                       out, out_len);
}

/** The entry plugin and name name, freed with free; NULL with the refusal already written to out. */
static char *checked_entry(const char *plugin, const char *name, int32_t *status, uint8_t **out, size_t *out_len) {
    if (!is_usable_plugin(plugin)) {
        *status = refuse_plugin(out, out_len);
        return NULL;
    }
    if (name == NULL || name[0] == '\0') {
        *status = refuse_name(out, out_len);
        return NULL;
    }
    char *entry = entry_name(plugin, name);
    *status = entry != NULL ? SPISOR_STATUS_SUCCESS : out_of_memory(out, out_len);
    return entry;
}

static char *requested_entry(const secret_request *request, int32_t *status, uint8_t **out, size_t *out_len) {
    return checked_entry(string_member(request->argument, "plugin"), string_member(request->argument, "name"), status,
                         out, out_len);
}

static int is_over_cap(const char *value) {
    return strlen(value) > RUNTIME_SECRET_VALUE_CAP_BYTES;
}

static cJSON *first_movable_entry(const cJSON *entries) {
    for (cJSON *item = entries->child; item != NULL; item = item->next) {
        if (cJSON_IsString(item) && !is_over_cap(item->valuestring)) {
            return item;
        }
    }
    return NULL;
}

/**
 * Moves every string entry of the file into the store, one at a time: the store write first, then
 * the file rewritten without that entry, so an interruption leaves an entry in both places at worst
 * and never in neither. Stops at the first entry the store does not take.
 *
 * @implNote An entry over the cap is skipped and stays in the file, where get still reads it. Only a
 * hand edit can put one there, since a set refuses it, and skipping it keeps it from stopping every
 * entry after it.
 */
static move_result move_file_secrets(const char *file_path) {
    move_result result = {RUNTIME_SECRET_FILE_DONE, 0};
    cJSON *entries = runtime_read_secret_file(file_path, &result.file);
    cJSON *entry;
    while (entries != NULL && (entry = first_movable_entry(entries)) != NULL &&
           runtime_secret_store_set(entry->string, entry->valuestring) == RUNTIME_SECRET_DONE) {
        cJSON_Delete(cJSON_DetachItemViaPointer(entries, entry));
        result.file = runtime_write_secret_file(file_path, entries);
        if (result.file != RUNTIME_SECRET_FILE_DONE) {
            break;
        }
        result.moved++;
    }
    cJSON_Delete(entries);
    return result;
}

static int32_t get_from_store(const secret_request *request, const char *entry, runtime_secret_file_answer file,
                              uint8_t **out, size_t *out_len) {
    char *value = NULL;
    runtime_secret_answer store =
        request->force_file_store ? RUNTIME_SECRET_UNAVAILABLE : runtime_secret_store_get(entry, &value);
    int32_t status;
    if (store == RUNTIME_SECRET_DONE) {
        status = answer_string("value", value, out, out_len);
    } else if (file != RUNTIME_SECRET_FILE_DONE) {
        status = refuse_file(request->secrets_file, file, out, out_len);
    } else if (store == RUNTIME_SECRET_FAILED || store == RUNTIME_SECRET_DECLINED) {
        status = refuse_store("read", store, out, out_len);
    } else {
        status = answer_string("value", NULL, out, out_len);
    }
    free(value);
    return status;
}

static int32_t get_entry(const secret_request *request, const char *entry, uint8_t **out, size_t *out_len) {
    runtime_secret_file_answer file;
    char *value = runtime_get_file_secret(request->secrets_file, entry, &file);
    int32_t status = value != NULL ? answer_string("value", value, out, out_len)
                                   : get_from_store(request, entry, file, out, out_len);
    free(value);
    return status;
}

static int32_t secrets_get(const secret_request *request, uint8_t **out, size_t *out_len) {
    int32_t status;
    char *entry = requested_entry(request, &status, out, out_len);
    if (entry == NULL) {
        return status;
    }
    status = get_entry(request, entry, out, out_len);
    free(entry);
    return status;
}

static int32_t put_in_file(const secret_request *request, const char *entry, const char *value, uint8_t **out,
                           size_t *out_len) {
    runtime_secret_file_answer file = runtime_put_file_secret(request->secrets_file, entry, value);
    return file == RUNTIME_SECRET_FILE_DONE ? answer_string("place", "file", out, out_len)
                                            : refuse_file(request->secrets_file, file, out, out_len);
}

static int32_t refuse_stale_copy(const char *file_path, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "the new value is in the store; remove the entry from %s by hand, then set again",
             file_path);
    return spisor_fail("the secret was stored, but its older copy in the secrets file could not be removed", fix, out,
                       out_len);
}

static int32_t settle_after_store(const secret_request *request, const char *entry, uint8_t **out, size_t *out_len) {
    runtime_secret_file_answer file = runtime_remove_file_secret(request->secrets_file, entry);
    if (file == RUNTIME_SECRET_FILE_UNWRITABLE || file == RUNTIME_SECRET_FILE_OUT_OF_MEMORY) {
        return refuse_stale_copy(request->secrets_file, out, out_len);
    }
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(request->secrets_file, file, out, out_len);
    }
    move_file_secrets(request->secrets_file);
    return answer_string("place", "store", out, out_len);
}

static runtime_secret_file_answer read_file_answer(const char *file_path) {
    runtime_secret_file_answer answer;
    cJSON_Delete(runtime_read_secret_file(file_path, &answer));
    return answer;
}

/**
 * @implNote The file is read before the store is written, so a file that cannot be read refuses the
 * set before either place holds the new value; refusing after the store write would leave a value
 * the answer calls unchanged, and the file's older copy would later overwrite it.
 */
static int32_t store_or_file(const secret_request *request, const char *entry, const char *value, uint8_t **out,
                             size_t *out_len) {
    runtime_secret_file_answer file = read_file_answer(request->secrets_file);
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(request->secrets_file, file, out, out_len);
    }
    if (request->force_file_store) {
        return put_in_file(request, entry, value, out, out_len);
    }
    runtime_secret_answer store = runtime_secret_store_set(entry, value);
    if (store == RUNTIME_SECRET_DONE) {
        return settle_after_store(request, entry, out, out_len);
    }
    if (store == RUNTIME_SECRET_DECLINED || store == RUNTIME_SECRET_UNAVAILABLE) {
        return put_in_file(request, entry, value, out, out_len);
    }
    return refuse_store("store", RUNTIME_SECRET_FAILED, out, out_len);
}

static int32_t refuse_over_cap(uint8_t **out, size_t *out_len) {
    char message[160];
    snprintf(message, sizeof message, "the value is longer than the %u bytes a secret may hold",
             RUNTIME_SECRET_VALUE_CAP_BYTES);
    return spisor_fail(message, "store a value of at most that many bytes of UTF-8; nothing was written", out,
                       out_len);
}

static int32_t store_text(const secret_request *request, const char *entry, const char *value, uint8_t **out,
                          size_t *out_len) {
    return is_over_cap(value) ? refuse_over_cap(out, out_len) : store_or_file(request, entry, value, out, out_len);
}

static int32_t store_value(const secret_request *request, const char *entry, uint8_t **out, size_t *out_len) {
    const char *value = string_member(request->argument, "value");
    if (value == NULL) {
        return spisor_fail("the argument carries no string value", "send {\"value\": \"<secret>\"}, a UTF-8 string",
                           out, out_len);
    }
    return store_text(request, entry, value, out, out_len);
}

static int32_t secrets_set(const secret_request *request, uint8_t **out, size_t *out_len) {
    int32_t status;
    char *entry = requested_entry(request, &status, out, out_len);
    if (entry == NULL) {
        return status;
    }
    status = store_value(request, entry, out, out_len);
    free(entry);
    return status;
}

static int store_kept_it(runtime_secret_answer answer) {
    return answer == RUNTIME_SECRET_DECLINED || answer == RUNTIME_SECRET_FAILED;
}

static int32_t remove_from_file(const secret_request *request, const char *entry, uint8_t **out, size_t *out_len) {
    runtime_secret_file_answer file = runtime_remove_file_secret(request->secrets_file, entry);
    return file == RUNTIME_SECRET_FILE_DONE ? answer_with(cJSON_CreateObject(), out, out_len)
                                            : refuse_file(request->secrets_file, file, out, out_len);
}

static int32_t delete_entry(const secret_request *request, const char *entry, uint8_t **out, size_t *out_len) {
    runtime_secret_answer store =
        request->force_file_store ? RUNTIME_SECRET_UNAVAILABLE : runtime_secret_store_delete(entry);
    return store_kept_it(store) ? refuse_store("remove", store, out, out_len)
                                : remove_from_file(request, entry, out, out_len);
}

static int32_t secrets_delete(const secret_request *request, uint8_t **out, size_t *out_len) {
    int32_t status;
    char *entry = requested_entry(request, &status, out, out_len);
    if (entry == NULL) {
        return status;
    }
    status = delete_entry(request, entry, out, out_len);
    free(entry);
    return status;
}

static runtime_secret_answer delete_store_entries_under(const char *prefix) {
    runtime_secret_names names = {NULL, 0};
    runtime_secret_answer answer = runtime_secret_store_list(prefix, &names);
    if (answer == RUNTIME_SECRET_UNAVAILABLE) {
        return RUNTIME_SECRET_DONE;
    }
    for (size_t index = 0; answer == RUNTIME_SECRET_DONE && index < names.count; index++) {
        runtime_secret_answer deleted =
            names.names[index] == NULL ? RUNTIME_SECRET_FAILED : runtime_secret_store_delete(names.names[index]);
        answer = deleted == RUNTIME_SECRET_ABSENT ? RUNTIME_SECRET_DONE : deleted;
    }
    runtime_free_secret_names(&names);
    return answer;
}

static int32_t clear_under(const secret_request *request, const char *prefix, uint8_t **out, size_t *out_len) {
    runtime_secret_answer store =
        request->force_file_store ? RUNTIME_SECRET_DONE : delete_store_entries_under(prefix);
    runtime_secret_file_answer file = runtime_remove_file_secrets_under(request->secrets_file, prefix);
    if (store != RUNTIME_SECRET_DONE) {
        return refuse_store("remove", store, out, out_len);
    }
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(request->secrets_file, file, out, out_len);
    }
    return answer_with(cJSON_CreateObject(), out, out_len);
}

static int32_t secrets_clear(const secret_request *request, uint8_t **out, size_t *out_len) {
    const char *plugin = string_member(request->argument, "plugin");
    if (!is_usable_plugin(plugin)) {
        return refuse_plugin(out, out_len);
    }
    char *prefix = entry_name(plugin, "");
    if (prefix == NULL) {
        return out_of_memory(out, out_len);
    }
    int32_t status = clear_under(request, prefix, out, out_len);
    free(prefix);
    return status;
}

static int32_t secrets_reconcile(const secret_request *request, uint8_t **out, size_t *out_len) {
    if (request->force_file_store || !runtime_secret_store_available()) {
        return answer_moved(0, out, out_len);
    }
    move_result result = move_file_secrets(request->secrets_file);
    return result.file == RUNTIME_SECRET_FILE_DONE ? answer_moved(result.moved, out, out_len)
                                                   : refuse_file(request->secrets_file, result.file, out, out_len);
}

int32_t runtime_refuse_secret_places(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable places",
                       "send {\"places\": {\"secretsFile\": \"<absolute path>\", \"forceFileStore\": <boolean>}} as "
                       "places.resolve answers them",
                       out, out_len);
}

int runtime_secret_place_of(const cJSON *argument, runtime_secret_place *place) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    const char *secrets_file = string_member(places, "secretsFile");
    const cJSON *force = cJSON_GetObjectItemCaseSensitive(places, "forceFileStore");
    if (secrets_file == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, secrets_file) ||
        !cJSON_IsBool(force)) {
        return 0;
    }
    place->secrets_file = secrets_file;
    place->force_file_store = cJSON_IsTrue(force);
    return 1;
}

static int read_places(const cJSON *argument, secret_request *request) {
    runtime_secret_place place;
    if (!runtime_secret_place_of(argument, &place)) {
        return 0;
    }
    request->secrets_file = place.secrets_file;
    request->force_file_store = place.force_file_store;
    request->argument = argument;
    return 1;
}

typedef enum entry_action { GET_ENTRY, SET_ENTRY, DELETE_ENTRY } entry_action;

static int32_t act_on_entry(const runtime_secret_place *place, const char *plugin, const char *name,
                            entry_action action, const char *value, uint8_t **out, size_t *out_len) {
    secret_request request = {place->secrets_file, place->force_file_store, NULL};
    int32_t status;
    char *entry = checked_entry(plugin, name, &status, out, out_len);
    if (entry == NULL) {
        return status;
    }
    status = action == GET_ENTRY   ? get_entry(&request, entry, out, out_len)
             : action == SET_ENTRY ? store_text(&request, entry, value, out, out_len)
                                   : delete_entry(&request, entry, out, out_len);
    free(entry);
    return status;
}

int32_t runtime_get_secret(const runtime_secret_place *place, const char *plugin, const char *name, uint8_t **out,
                           size_t *out_len) {
    return act_on_entry(place, plugin, name, GET_ENTRY, NULL, out, out_len);
}

int32_t runtime_set_secret(const runtime_secret_place *place, const char *plugin, const char *name, const char *value,
                           uint8_t **out, size_t *out_len) {
    return act_on_entry(place, plugin, name, SET_ENTRY, value, out, out_len);
}

int32_t runtime_delete_secret(const runtime_secret_place *place, const char *plugin, const char *name, uint8_t **out,
                              size_t *out_len) {
    return act_on_entry(place, plugin, name, DELETE_ENTRY, NULL, out, out_len);
}

static int32_t dispatch(const uint8_t *in, size_t in_len, secret_operation operation, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    secret_request request;
    status = read_places(call.argument, &request) ? operation(&request, out, out_len)
                                                  : runtime_refuse_secret_places(out, out_len);
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_secrets_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, secrets_get, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_secrets_set(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, secrets_set, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_secrets_delete(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, secrets_delete, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_secrets_clear(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, secrets_clear, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_secrets_reconcile(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return dispatch(in, in_len, secrets_reconcile, out, out_len);
}
