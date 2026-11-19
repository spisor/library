#include "secrets.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "definition_support.h"
#include "paths.h"
#include "secret_backend.h"
#include "secret_file.h"
#include "spisor/path.h"

typedef struct move_result {
    runtime_secret_file_answer file;
    int moved;
} move_result;

void runtime_free_secret_names(runtime_secret_names *names) {
    for (size_t index = 0; names->names != NULL && index < names->count; index++) {
        spisor_memory_release(names->names[index]);
    }
    spisor_memory_release(names->names);
    names->names = NULL;
    names->count = 0;
}

static spisor_status refuse_file(const char *file_path, runtime_secret_file_answer answer, spisor_refusal *refusal) {
    char fix[2048];
    if (answer == RUNTIME_SECRET_FILE_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (answer == RUNTIME_SECRET_FILE_UNREADABLE) {
        snprintf(fix, sizeof fix, "repair %s so it holds one JSON object of strings, or remove it; it was left unchanged",
                 file_path);
        return spisor_refuse_as(refusal, SPISOR_REFUSED, "the secrets file is not a readable JSON object", fix);
    }
    if (answer == RUNTIME_SECRET_FILE_LINKED) {
        snprintf(fix, sizeof fix,
                 "replace the link at %s with a regular file, or set SPISOR_SECRETS to a private directory to keep "
                 "the secrets file elsewhere; it was left unchanged",
                 file_path);
        return spisor_refuse_as(refusal, SPISOR_REFUSED,
                                 "the secrets file is a symbolic link, and a secret is never written through one", fix);
    }
    snprintf(fix, sizeof fix, "check that %s and its directory can be written", file_path);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, "the secrets file could not be written", fix);
}

static spisor_status refuse_store(const char *action, runtime_secret_answer answer, spisor_refusal *refusal) {
    char detail[256];
    snprintf(detail, sizeof detail, "the OS credential store %s while asked to %s the secret",
             answer == RUNTIME_SECRET_DECLINED ? "declined" : "failed", action);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail,
                             "unlock the credential store or allow this program to use it, then try again");
}

static int is_usable_plugin(const char *plugin) {
    return plugin != NULL && plugin[0] != '\0' && strchr(plugin, '/') == NULL;
}

static char *entry_name(const char *plugin, const char *name) {
    size_t size = sizeof("spisor//") + strlen(plugin) + strlen(name);
    char *entry = spisor_memory_allocate(size);
    if (entry != NULL) {
        snprintf(entry, size, "spisor/%s/%s", plugin, name);
    }
    return entry;
}

static spisor_status refuse_plugin(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no usable plugin",
                             "send plugin as the manifest id, a non-empty id without a slash");
}

static spisor_status refuse_name(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no usable name",
                             "send name as the secret's name, a non-empty string");
}

/** The entry plugin and name name, freed; NULL with *status set to the refusal or OUT_OF_MEMORY. */
static char *checked_entry(const char *plugin, const char *name, spisor_status *status, spisor_refusal *refusal) {
    char *entry;
    if (!is_usable_plugin(plugin)) {
        *status = refuse_plugin(refusal);
        return NULL;
    }
    if (name == NULL || name[0] == '\0') {
        *status = refuse_name(refusal);
        return NULL;
    }
    entry = entry_name(plugin, name);
    *status = entry != NULL ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    return entry;
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

static spisor_status get_from_store(const runtime_secret_places *place, const char *entry,
                                    runtime_secret_file_answer file, char **value, spisor_refusal *refusal) {
    char *stored = NULL;
    runtime_secret_answer store =
        place->force_file_store ? RUNTIME_SECRET_UNAVAILABLE : runtime_secret_store_get(entry, &stored);
    if (store == RUNTIME_SECRET_DONE) {
        *value = stored;
        return SPISOR_OK;
    }
    spisor_memory_release(stored);
    if (store == RUNTIME_SECRET_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(place->secrets_file, file, refusal);
    }
    if (store == RUNTIME_SECRET_FAILED || store == RUNTIME_SECRET_DECLINED) {
        return refuse_store("read", store, refusal);
    }
    return SPISOR_NONE;
}

static spisor_status get_entry(const runtime_secret_places *place, const char *entry, char **value,
                               spisor_refusal *refusal) {
    runtime_secret_file_answer file;
    char *held = runtime_get_file_secret(place->secrets_file, entry, &file);
    if (held != NULL) {
        *value = held;
        return SPISOR_OK;
    }
    return get_from_store(place, entry, file, value, refusal);
}

static spisor_status put_in_file(const runtime_secret_places *place, const char *entry, const char *value,
                                 runtime_secret_location *location, spisor_refusal *refusal) {
    runtime_secret_file_answer file = runtime_put_file_secret(place->secrets_file, entry, value);
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(place->secrets_file, file, refusal);
    }
    *location = RUNTIME_SECRET_LOCATION_FILE;
    return SPISOR_OK;
}

static spisor_status refuse_stale_copy(const char *file_path, spisor_refusal *refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "the new value is in the store; remove the entry from %s by hand, then set again",
             file_path);
    return spisor_refuse_as(refusal, SPISOR_REFUSED,
                             "the secret was stored, but its older copy in the secrets file could not be removed", fix);
}

static spisor_status settle_after_store(const runtime_secret_places *place, const char *entry,
                                        runtime_secret_location *location, spisor_refusal *refusal) {
    runtime_secret_file_answer file = runtime_remove_file_secret(place->secrets_file, entry);
    if (file == RUNTIME_SECRET_FILE_UNWRITABLE || file == RUNTIME_SECRET_FILE_OUT_OF_MEMORY) {
        return refuse_stale_copy(place->secrets_file, refusal);
    }
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(place->secrets_file, file, refusal);
    }
    move_file_secrets(place->secrets_file);
    *location = RUNTIME_SECRET_LOCATION_STORE;
    return SPISOR_OK;
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
static spisor_status store_or_file(const runtime_secret_places *place, const char *entry, const char *value,
                                   runtime_secret_location *location, spisor_refusal *refusal) {
    runtime_secret_file_answer file = read_file_answer(place->secrets_file);
    runtime_secret_answer store;
    if (file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(place->secrets_file, file, refusal);
    }
    if (place->force_file_store) {
        return put_in_file(place, entry, value, location, refusal);
    }
    store = runtime_secret_store_set(entry, value);
    if (store == RUNTIME_SECRET_DONE) {
        return settle_after_store(place, entry, location, refusal);
    }
    if (store == RUNTIME_SECRET_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (store == RUNTIME_SECRET_DECLINED || store == RUNTIME_SECRET_UNAVAILABLE) {
        return put_in_file(place, entry, value, location, refusal);
    }
    return refuse_store("store", RUNTIME_SECRET_FAILED, refusal);
}

static spisor_status refuse_over_cap(spisor_refusal *refusal) {
    char detail[160];
    snprintf(detail, sizeof detail, "the value is longer than the %u bytes a secret may hold",
             RUNTIME_SECRET_VALUE_CAP_BYTES);
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail,
                             "store a value of at most that many bytes of UTF-8; nothing was written");
}

static int store_kept_it(runtime_secret_answer answer) {
    return answer == RUNTIME_SECRET_DECLINED || answer == RUNTIME_SECRET_FAILED;
}

static spisor_status remove_from_file(const runtime_secret_places *place, const char *entry, spisor_refusal *refusal) {
    runtime_secret_file_answer file = runtime_remove_file_secret(place->secrets_file, entry);
    return file == RUNTIME_SECRET_FILE_DONE ? SPISOR_OK : refuse_file(place->secrets_file, file, refusal);
}

static spisor_status delete_entry(const runtime_secret_places *place, const char *entry, spisor_refusal *refusal) {
    runtime_secret_answer store =
        place->force_file_store ? RUNTIME_SECRET_UNAVAILABLE : runtime_secret_store_delete(entry);
    if (store == RUNTIME_SECRET_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    return store_kept_it(store) ? refuse_store("remove", store, refusal) : remove_from_file(place, entry, refusal);
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

static spisor_status clear_under(const runtime_secret_places *place, const char *prefix, spisor_refusal *refusal) {
    runtime_secret_answer store = place->force_file_store ? RUNTIME_SECRET_DONE : delete_store_entries_under(prefix);
    if (store == RUNTIME_SECRET_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    runtime_secret_file_answer file = runtime_remove_file_secrets_under(place->secrets_file, prefix);
    if (store != RUNTIME_SECRET_DONE) {
        return refuse_store("remove", store, refusal);
    }
    return file == RUNTIME_SECRET_FILE_DONE ? SPISOR_OK : refuse_file(place->secrets_file, file, refusal);
}

int runtime_is_usable_secret_place(const runtime_secret_places *place) {
    return place != NULL && spisor_has_shape(place->size, SPISOR_SHAPE_THROUGH(runtime_secret_places, force_file_store)) &&
           place->secrets_file != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place->secrets_file);
}

spisor_status runtime_refuse_secret_place(spisor_refusal *refusal) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no usable places",
                             "send places with secrets_file and force_file_store as places' resolve answers them, the "
                             "secrets file an absolute path");
}

spisor_status runtime_get_secret(const runtime_secret_places *place, const char *plugin, const char *name, char **value,
                                 spisor_refusal *refusal) {
    spisor_status status;
    char *entry = checked_entry(plugin, name, &status, refusal);
    *value = NULL;
    if (entry == NULL) {
        return status;
    }
    status = get_entry(place, entry, value, refusal);
    spisor_memory_release(entry);
    return status;
}

spisor_status runtime_set_secret(const runtime_secret_places *place, const char *plugin, const char *name,
                                 const char *value, runtime_secret_location *location, spisor_refusal *refusal) {
    spisor_status status;
    char *entry = checked_entry(plugin, name, &status, refusal);
    if (entry == NULL) {
        return status;
    }
    if (value == NULL) {
        status = spisor_refuse_as(refusal, SPISOR_MALFORMED, "the argument carries no value",
                                   "send value as the secret, a UTF-8 string");
    } else {
        status = is_over_cap(value) ? refuse_over_cap(refusal) : store_or_file(place, entry, value, location, refusal);
    }
    spisor_memory_release(entry);
    return status;
}

spisor_status runtime_delete_secret(const runtime_secret_places *place, const char *plugin, const char *name,
                                    spisor_refusal *refusal) {
    spisor_status status;
    char *entry = checked_entry(plugin, name, &status, refusal);
    if (entry == NULL) {
        return status;
    }
    status = delete_entry(place, entry, refusal);
    spisor_memory_release(entry);
    return status;
}

spisor_status runtime_clear_secrets(const runtime_secret_places *place, const char *plugin, spisor_refusal *refusal) {
    spisor_status status;
    char *prefix;
    if (!is_usable_plugin(plugin)) {
        return refuse_plugin(refusal);
    }
    prefix = entry_name(plugin, "");
    if (prefix == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = clear_under(place, prefix, refusal);
    spisor_memory_release(prefix);
    return status;
}

spisor_status runtime_reconcile_secrets(const runtime_secret_places *place, uint64_t *moved, spisor_refusal *refusal) {
    move_result result;
    *moved = 0;
    if (place->force_file_store || !runtime_secret_store_available()) {
        return SPISOR_OK;
    }
    result = move_file_secrets(place->secrets_file);
    if (result.file != RUNTIME_SECRET_FILE_DONE) {
        return refuse_file(place->secrets_file, result.file, refusal);
    }
    *moved = (uint64_t)result.moved;
    return SPISOR_OK;
}
