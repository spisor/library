#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_resolve.h"
#include "directory_tree.h"
#include "hosts_lock.h"
#include "file_presence.h"
#include "json_service.h"
#include "paths.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define REGISTRATION_SUFFIX ".json"
#define REGISTRATION_SUFFIX_LENGTH 5

typedef struct registry_request {
    const cJSON *argument;
    const char *state;
    const char *cache;
} registry_request;

typedef int32_t (*registry_handler)(const registry_request *request, uint8_t **out, size_t *out_len);

static int32_t out_of_memory(uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    return SPISOR_STATUS_OUT_OF_MEMORY;
}

static int32_t answer_with(cJSON *result, uint8_t **out, size_t *out_len) {
    if (result == NULL) {
        return out_of_memory(out, out_len);
    }
    int32_t status = spisor_succeed(result, out, out_len);
    cJSON_Delete(result);
    return status;
}

static char *join_two(const char *base, const char *name) {
    const char *segments[2] = {base, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static char *join_three(const char *base, const char *middle, const char *name) {
    const char *segments[3] = {base, middle, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 3);
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static int is_host_id(const char *id) {
    char *segment = runtime_portable_segment(id);
    int portable = segment != NULL && strcmp(segment, id) == 0;
    free(segment);
    return portable;
}

static char *registration_path(const char *state, const char *id) {
    char *hosts = join_two(state, "hosts");
    char *file_name = malloc(strlen(id) + REGISTRATION_SUFFIX_LENGTH + 1);
    char *path = NULL;
    if (hosts != NULL && file_name != NULL) {
        snprintf(file_name, strlen(id) + REGISTRATION_SUFFIX_LENGTH + 1, "%s%s", id, REGISTRATION_SUFFIX);
        path = join_two(hosts, file_name);
    }
    free(hosts);
    free(file_name);
    return path;
}

static const char *string_member(const cJSON *object, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int is_list_or_absent(const cJSON *registration, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(registration, member);
    return item == NULL || cJSON_IsArray(item);
}

static int is_usable_stored_record(const cJSON *record, const char *id) {
    const char *stored_id = string_member(record, "id");
    return cJSON_IsObject(record) && stored_id != NULL && strcmp(stored_id, id) == 0 &&
           is_list_or_absent(record, "commands") && is_list_or_absent(record, "changes");
}

runtime_registry_answer runtime_registry_load(const char *state, const char *id, cJSON **registration) {
    *registration = NULL;
    if (!is_host_id(id)) {
        return RUNTIME_REGISTRY_INVALID;
    }
    char *path = registration_path(state, id);
    if (path == NULL) {
        return RUNTIME_REGISTRY_OUT_OF_MEMORY;
    }
    spisor_json_file_state file_state;
    cJSON *record = spisor_read_json_file(path, &file_state);
    free(path);
    if (file_state == SPISOR_JSON_FILE_ABSENT) {
        return RUNTIME_REGISTRY_ABSENT;
    }
    if (file_state != SPISOR_JSON_FILE_PARSED || !is_usable_stored_record(record, id)) {
        cJSON_Delete(record);
        return RUNTIME_REGISTRY_UNREADABLE;
    }
    *registration = record;
    return RUNTIME_REGISTRY_OK;
}

static uint8_t *registration_bytes(const cJSON *registration, size_t *length) {
    char *text = spisor_json_stringify_pretty(registration);
    if (text == NULL) {
        return NULL;
    }
    size_t text_length = strlen(text);
    char *terminated = realloc(text, text_length + 2);
    if (terminated == NULL) {
        free(text);
        return NULL;
    }
    terminated[text_length] = '\n';
    terminated[text_length + 1] = '\0';
    *length = text_length + 1;
    return (uint8_t *)terminated;
}

static int make_hosts_directory(const char *state) {
    char *hosts = join_two(state, "hosts");
    int made = hosts != NULL && spisor_make_directories(hosts);
    free(hosts);
    return made;
}

runtime_registry_answer runtime_registry_store(const char *state, const cJSON *registration) {
    const char *id = string_member(registration, "id");
    if (id == NULL || !is_host_id(id)) {
        return RUNTIME_REGISTRY_INVALID;
    }
    char *path = registration_path(state, id);
    size_t length = 0;
    uint8_t *bytes = path == NULL ? NULL : registration_bytes(registration, &length);
    if (bytes == NULL) {
        free(path);
        return RUNTIME_REGISTRY_OUT_OF_MEMORY;
    }
    int written = make_hosts_directory(state) && spisor_replace_file(path, bytes, length);
    free(path);
    free(bytes);
    return written ? RUNTIME_REGISTRY_OK : RUNTIME_REGISTRY_UNWRITABLE;
}

#define UNREADABLE_MESSAGE "the host's registration is not a readable JSON object for that id"

static void unreadable_fix(const char *state, const char *id, char *fix, size_t size) {
    char *path = registration_path(state, id);
    snprintf(fix, size, "repair %s so it holds the host's registration as one JSON object, or remove it; it was left unchanged",
             path != NULL ? path : "the registration file");
    free(path);
}

static int32_t refuse_unreadable(const char *state, const char *id, uint8_t **out, size_t *out_len) {
    char fix[2048];
    unreadable_fix(state, id, fix, sizeof fix);
    return spisor_fail(UNREADABLE_MESSAGE, fix, out, out_len);
}

int runtime_registration_usable(const cJSON *registration, const char *id, const char *state, cJSON **refusal) {
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(registration, "version")) &&
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(registration, "home"))) {
        return 1;
    }
    const char *format = "the host %s is not registered in %s";
    size_t size = strlen(format) + strlen(id) + strlen(state) + 1;
    char *message = malloc(size);
    if (message != NULL) {
        snprintf(message, size, format, id, state);
        *refusal = runtime_refusal(message, RUNTIME_REINSTALL_FIX);
    }
    free(message);
    return 0;
}

cJSON *runtime_usable_registration(const char *state, const char *id, cJSON **refusal) {
    cJSON *registration = NULL;
    runtime_registry_answer loaded = runtime_registry_load(state, id, &registration);
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        char fix[2048];
        unreadable_fix(state, id, fix, sizeof fix);
        *refusal = runtime_refusal(UNREADABLE_MESSAGE, fix);
        return NULL;
    }
    if (loaded == RUNTIME_REGISTRY_OUT_OF_MEMORY) {
        return NULL;
    }
    if (!runtime_registration_usable(registration, id, state, refusal)) {
        cJSON_Delete(registration);
        return NULL;
    }
    return registration;
}

static int32_t refuse_id(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable host id",
                       "send {\"id\": \"<host id>\"}, a portable path segment with no surrounding whitespace", out,
                       out_len);
}

static int32_t refuse_unwritable(const char *state, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts can be created and written", state);
    return spisor_fail("the host's registration could not be written", fix, out, out_len);
}

static int32_t refuse_answer(runtime_registry_answer answer, const registry_request *request, const char *id,
                             uint8_t **out, size_t *out_len) {
    switch (answer) {
    case RUNTIME_REGISTRY_UNREADABLE:
        return refuse_unreadable(request->state, id, out, out_len);
    case RUNTIME_REGISTRY_INVALID:
        return refuse_id(out, out_len);
    case RUNTIME_REGISTRY_UNWRITABLE:
        return refuse_unwritable(request->state, out, out_len);
    default:
        return out_of_memory(out, out_len);
    }
}

static int has_text(const cJSON *object, const char *member) {
    const char *text = string_member(object, member);
    return text != NULL && text[0] != 0;
}

static int is_digit_run(const char *text, size_t start, size_t count) {
    for (size_t index = start; index < start + count; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
    }
    return 1;
}

static int is_fraction_then_zone(const char *text) {
    if (*text == '.') {
        text++;
        if (*text < '0' || *text > '9') {
            return 0;
        }
        while (*text >= '0' && *text <= '9') {
            text++;
        }
    }
    return strcmp(text, "Z") == 0;
}

/**
 * Whether text has the shape of an ISO 8601 UTC timestamp, YYYY-MM-DDTHH:MM:SS with an optional
 * fraction and a closing Z, as JavaScript's toISOString writes it.
 *
 * @implNote Only the shape is checked, never the calendar: the stamp is a record of when, read by
 * people and by an update, and nothing computes with it.
 */
static int is_utc_timestamp(const char *text) {
    if (text == NULL || strlen(text) < 20) {
        return 0;
    }
    return is_digit_run(text, 0, 4) && text[4] == '-' && is_digit_run(text, 5, 2) && text[7] == '-' &&
           is_digit_run(text, 8, 2) && text[10] == 'T' && is_digit_run(text, 11, 2) && text[13] == ':' &&
           is_digit_run(text, 14, 2) && text[16] == ':' && is_digit_run(text, 17, 2) &&
           is_fraction_then_zone(text + 19);
}

static const char *provenance_defect_of(const cJSON *provenance) {
    if (!cJSON_IsObject(provenance)) {
        return "the registration carries no provenance object";
    }
    if (!has_text(provenance, "method") || !has_text(provenance, "version")) {
        return "the registration provenance needs a method and a version, both strings";
    }
    if (!has_text(provenance, "location") ||
        !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, string_member(provenance, "location"))) {
        return "the registration provenance location is not an absolute path";
    }
    if (!has_text(provenance, "os") || !has_text(provenance, "architecture")) {
        return "the registration provenance needs an os and an architecture, both non-empty strings";
    }
    if (!is_utc_timestamp(string_member(provenance, "installedAt"))) {
        return "the registration provenance installedAt is not an ISO 8601 UTC timestamp like 2026-11-02T10:00:00Z";
    }
    return NULL;
}

static int is_start_runtime(const char *runtime) {
    return runtime != NULL &&
           (strcmp(runtime, "javascript") == 0 || strcmp(runtime, "jvm") == 0 || strcmp(runtime, "native") == 0);
}

static int climbs_out(const char *entry) {
    const char *segment = entry;
    while (*segment != '\0') {
        size_t length = strcspn(segment, "/\\");
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            return 1;
        }
        segment += length;
        segment += *segment != '\0';
    }
    return 0;
}

static const char *start_defect_of(const cJSON *start) {
    if (start == NULL) {
        return NULL;
    }
    if (!cJSON_IsObject(start) || !is_start_runtime(string_member(start, "runtime"))) {
        return "the registration's start needs a runtime of \"javascript\", \"jvm\" or \"native\"";
    }
    const char *entry = string_member(start, "entry");
    if (entry == NULL || entry[0] == '\0' || spisor_path_is_absolute(1, entry) || spisor_path_is_absolute(0, entry) ||
        climbs_out(entry)) {
        return "the registration's start entry is not a relative path inside the host's bundle";
    }
    return NULL;
}

static const char *registration_defect(const cJSON *registration) {
    const char *id = string_member(registration, "id");
    const cJSON *home = cJSON_GetObjectItemCaseSensitive(registration, "home");
    const cJSON *application = cJSON_GetObjectItemCaseSensitive(registration, "application");
    if (!cJSON_IsObject(registration)) {
        return "the registration is not an object";
    }
    if (id == NULL || !is_host_id(id)) {
        return "the registration carries no usable id";
    }
    if (string_member(registration, "kind") == NULL || strcmp(string_member(registration, "kind"), "host") != 0) {
        return "the registration's kind is not \"host\"";
    }
    if (string_member(registration, "version") == NULL || string_member(registration, "version")[0] == '\0') {
        return "the registration carries no version";
    }
    if (home != NULL && !cJSON_IsString(home)) {
        return "the registration's home is not a string";
    }
    if (application != NULL && !cJSON_IsObject(application)) {
        return "the registration's application is not an object";
    }
    const char *provenance_defect = provenance_defect_of(cJSON_GetObjectItemCaseSensitive(registration, "provenance"));
    if (provenance_defect != NULL) {
        return provenance_defect;
    }
    if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(registration, "requiresCommand"))) {
        return "the registration's requiresCommand is not a boolean";
    }
    return start_defect_of(cJSON_GetObjectItemCaseSensitive(registration, "start"));
}

static int32_t refuse_registration(const char *defect, uint8_t **out, size_t *out_len) {
    return spisor_fail(defect,
                       "send {\"registration\": {\"id\", \"kind\": \"host\", \"version\", \"home\"?, \"application\"?, "
                       "\"provenance\": {\"method\", \"version\", \"location\": <absolute path>, \"os\", \"architecture\", "
                       "\"installedAt\": <ISO 8601 UTC>, ...}, "
                       "\"requiresCommand\", \"start\"?: {\"runtime\": \"javascript\" | \"jvm\" | \"native\", "
                       "\"entry\": <relative path inside the bundle>}}}",
                       out, out_len);
}

static int add_copy(cJSON *target, const cJSON *source, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(source, member);
    if (item == NULL) {
        return 1;
    }
    cJSON *copy = cJSON_Duplicate(item, 1);
    if (copy != NULL && !cJSON_AddItemToObject(target, member, copy)) {
        cJSON_Delete(copy);
        copy = NULL;
    }
    return copy != NULL;
}

static int add_stored_list(cJSON *target, const cJSON *stored, const char *member) {
    cJSON *list = cJSON_GetObjectItemCaseSensitive(stored, member) != NULL
                      ? cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(stored, member), 1)
                      : cJSON_CreateArray();
    if (list != NULL && !cJSON_AddItemToObject(target, member, list)) {
        cJSON_Delete(list);
        list = NULL;
    }
    return list != NULL;
}

static const cJSON *provenance_source(const cJSON *given, const cJSON *stored, int keep_stored_provenance) {
    int keeps = keep_stored_provenance && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(stored, "provenance"));
    return keeps ? stored : given;
}

/**
 * The record to store: the given registration's own fields, with commands, changes and the
 * changes service's nextSequence counter carried over from stored (empty when stored is NULL), and
 * the stored provenance in place of the given one when keep_stored_provenance asks for it and stored
 * has one. Anything else in the given registration or the stored one is dropped, so the file never
 * holds a member nothing owns.
 *
 * @implNote Commands and changes are owned by the services that record them, never by a host
 * registering itself, so a host's start cannot erase what an install or a command left behind.
 */
static cJSON *merged_registration(const cJSON *given, const cJSON *stored, int keep_stored_provenance) {
    cJSON *merged = cJSON_CreateObject();
    const cJSON *provenance = provenance_source(given, stored, keep_stored_provenance);
    int complete = merged != NULL && add_copy(merged, given, "id") && add_copy(merged, given, "kind") &&
                   add_copy(merged, given, "version") && add_copy(merged, given, "home") &&
                   add_copy(merged, given, "application") && add_copy(merged, provenance, "provenance") &&
                   add_copy(merged, given, "requiresCommand") && add_copy(merged, given, "start") &&
                   add_stored_list(merged, stored, "commands") &&
                   add_stored_list(merged, stored, "changes") && add_copy(merged, stored, "nextSequence");
    if (!complete) {
        cJSON_Delete(merged);
        return NULL;
    }
    return merged;
}

static int32_t store_and_answer(const registry_request *request, cJSON *merged, uint8_t **out, size_t *out_len) {
    runtime_registry_answer stored = runtime_registry_store(request->state, merged);
    if (stored != RUNTIME_REGISTRY_OK) {
        int32_t status = refuse_answer(stored, request, string_member(merged, "id"), out, out_len);
        cJSON_Delete(merged);
        return status;
    }
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && !cJSON_AddItemToObject(result, "registration", merged)) {
        cJSON_Delete(result);
        result = NULL;
    }
    if (result == NULL) {
        cJSON_Delete(merged);
    }
    return answer_with(result, out, out_len);
}

static int32_t refuse_unlocked(const char *state, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts/.lock can be created, opened and locked", state);
    return spisor_fail("the registry lock could not be taken; nothing was changed", fix, out, out_len);
}

static int32_t register_locked(const registry_request *request, const cJSON *given, int keep_stored_provenance,
                               uint8_t **out, size_t *out_len) {
    const char *id = string_member(given, "id");
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, out, out_len);
    }
    cJSON *merged = merged_registration(given, stored, keep_stored_provenance);
    cJSON_Delete(stored);
    if (merged == NULL) {
        return out_of_memory(out, out_len);
    }
    return store_and_answer(request, merged, out, out_len);
}

static int32_t register_with(const registry_request *request, int keep_stored_provenance, uint8_t **out,
                             size_t *out_len) {
    const cJSON *given = cJSON_GetObjectItemCaseSensitive(request->argument, "registration");
    const char *defect = registration_defect(given);
    if (defect != NULL) {
        return refuse_registration(defect, out, out_len);
    }
    spisor_lock *guard = runtime_hosts_lock(request->state);
    if (guard == NULL) {
        return refuse_unlocked(request->state, out, out_len);
    }
    int32_t status = register_locked(request, given, keep_stored_provenance, out, out_len);
    spisor_unlock(guard);
    return status;
}

static int32_t register_host(const registry_request *request, uint8_t **out, size_t *out_len) {
    return register_with(request, 0, out, out_len);
}

static int32_t register_host_keeping_provenance(const registry_request *request, uint8_t **out, size_t *out_len) {
    return register_with(request, 1, out, out_len);
}

static int32_t get_host(const registry_request *request, uint8_t **out, size_t *out_len) {
    const char *id = string_member(request->argument, "id");
    if (id == NULL) {
        return refuse_id(out, out_len);
    }
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, out, out_len);
    }
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && stored != NULL && !cJSON_AddItemToObject(result, "registration", stored)) {
        cJSON_Delete(result);
        result = NULL;
    }
    if (result == NULL) {
        cJSON_Delete(stored);
    }
    return answer_with(result, out, out_len);
}

static int is_orphan(const cJSON *registration) {
    const cJSON *provenance = cJSON_GetObjectItemCaseSensitive(registration, "provenance");
    const char *location = string_member(provenance, "location");
    return location == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, location) ||
           spisor_probe_file(location) == SPISOR_FILE_MISSING;
}

static cJSON *listed_host(cJSON *registration) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL || !cJSON_AddItemToObject(entry, "registration", registration)) {
        cJSON_Delete(entry);
        cJSON_Delete(registration);
        return NULL;
    }
    if (cJSON_AddBoolToObject(entry, "orphan", is_orphan(registration)) == NULL) {
        cJSON_Delete(entry);
        return NULL;
    }
    return entry;
}

static int is_registration_file(const runtime_directory_entry *entry) {
    size_t length = strlen(entry->name);
    return !entry->is_directory && length > REGISTRATION_SUFFIX_LENGTH &&
           strcmp(entry->name + length - REGISTRATION_SUFFIX_LENGTH, REGISTRATION_SUFFIX) == 0;
}

static char *id_of_file(const char *file_name) {
    char *id = copy_text(file_name);
    if (id != NULL) {
        id[strlen(id) - REGISTRATION_SUFFIX_LENGTH] = '\0';
    }
    return id;
}

static int add_listed(cJSON *hosts, cJSON *registration) {
    cJSON *entry = listed_host(registration);
    if (entry != NULL && !cJSON_AddItemToArray(hosts, entry)) {
        cJSON_Delete(entry);
        entry = NULL;
    }
    return entry != NULL;
}

static int add_unreadable(cJSON *unreadable, const char *id) {
    cJSON *name = cJSON_CreateString(id);
    if (name != NULL && !cJSON_AddItemToArray(unreadable, name)) {
        cJSON_Delete(name);
        name = NULL;
    }
    return name != NULL;
}

static int list_one(const registry_request *request, const char *id, cJSON *hosts, cJSON *unreadable) {
    cJSON *registration = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &registration);
    if (loaded == RUNTIME_REGISTRY_OK) {
        return add_listed(hosts, registration);
    }
    if (loaded == RUNTIME_REGISTRY_UNREADABLE) {
        return add_unreadable(unreadable, id);
    }
    return loaded != RUNTIME_REGISTRY_OUT_OF_MEMORY;
}

static int list_each(const registry_request *request, const runtime_directory_listing *listing, cJSON *hosts,
                     cJSON *unreadable) {
    int complete = 1;
    for (size_t index = 0; complete && index < listing->count; index++) {
        if (is_registration_file(&listing->entries[index])) {
            char *id = id_of_file(listing->entries[index].name);
            complete = id != NULL && (!is_host_id(id) || list_one(request, id, hosts, unreadable));
            free(id);
        }
    }
    return complete;
}

static int32_t refuse_unlistable(const char *state, uint8_t **out, size_t *out_len) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts is a readable directory", state);
    return spisor_fail("the registry directory could not be read", fix, out, out_len);
}

static int32_t list_hosts(const registry_request *request, uint8_t **out, size_t *out_len) {
    char *directory = join_two(request->state, "hosts");
    runtime_directory_listing listing;
    if (directory == NULL) {
        return out_of_memory(out, out_len);
    }
    int listed = runtime_list_directory(directory, &listing);
    free(directory);
    if (!listed) {
        return refuse_unlistable(request->state, out, out_len);
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *hosts = result == NULL ? NULL : cJSON_AddArrayToObject(result, "hosts");
    cJSON *unreadable = hosts == NULL ? NULL : cJSON_AddArrayToObject(result, "unreadable");
    int complete = unreadable != NULL && list_each(request, &listing, hosts, unreadable);
    runtime_free_directory_listing(&listing);
    if (!complete) {
        cJSON_Delete(result);
        result = NULL;
    }
    return answer_with(result, out, out_len);
}

static int is_unreversed(const cJSON *change) {
    return !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(change, "reversed"));
}

static int count_unreversed(const cJSON *registration) {
    int count = 0;
    const cJSON *change;
    cJSON_ArrayForEach(change, cJSON_GetObjectItemCaseSensitive(registration, "changes")) {
        count += is_unreversed(change);
    }
    return count;
}

static int32_t refuse_unreversed(const char *id, int count, uint8_t **out, size_t *out_len) {
    char message[512];
    snprintf(message, sizeof message, "the host %s still holds %d change(s) that were not reversed", id, count);
    return spisor_fail(message, "reverse the host's recorded changes first; its registration was left in place", out,
                       out_len);
}

static int remove_host_directory(const char *place, const char *id) {
    char *directory = join_three(place, "hosts", id);
    int removed = directory != NULL && runtime_remove_tree(directory);
    free(directory);
    return removed;
}

static int remove_registration_file(const char *state, const char *id) {
    char *path = registration_path(state, id);
    int removed = path != NULL && runtime_remove_tree(path);
    free(path);
    return removed;
}

static int32_t refuse_incomplete_removal(const char *id, uint8_t **out, size_t *out_len) {
    char message[512];
    snprintf(message, sizeof message, "the host %s could not be removed completely", id);
    return spisor_fail(message, "close whatever holds its files open and forget it again; the registration is deleted "
                                "last, so nothing was orphaned",
                       out, out_len);
}

static int32_t answer_forgotten(int forgotten, uint8_t **out, size_t *out_len) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && cJSON_AddBoolToObject(result, "forgotten", forgotten) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return answer_with(result, out, out_len);
}

static int32_t forget_locked(const registry_request *request, const char *id, uint8_t **out, size_t *out_len) {
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, out, out_len);
    }
    int unreversed = count_unreversed(stored);
    cJSON_Delete(stored);
    if (unreversed > 0) {
        return refuse_unreversed(id, unreversed, out, out_len);
    }
    int removed = remove_host_directory(request->state, id) && remove_host_directory(request->cache, id) &&
                  remove_registration_file(request->state, id);
    return removed ? answer_forgotten(loaded == RUNTIME_REGISTRY_OK, out, out_len)
                   : refuse_incomplete_removal(id, out, out_len);
}

static int32_t forget_host(const registry_request *request, uint8_t **out, size_t *out_len) {
    const char *id = string_member(request->argument, "id");
    if (id == NULL) {
        return refuse_id(out, out_len);
    }
    spisor_lock *guard = runtime_hosts_lock(request->state);
    if (guard == NULL) {
        return refuse_unlocked(request->state, out, out_len);
    }
    int32_t status = forget_locked(request, id, out, out_len);
    spisor_unlock(guard);
    return status;
}

static int32_t forget_host_locked(const registry_request *request, uint8_t **out, size_t *out_len) {
    const char *id = string_member(request->argument, "id");
    return id == NULL ? refuse_id(out, out_len) : forget_locked(request, id, out, out_len);
}

static int32_t refuse_places(uint8_t **out, size_t *out_len) {
    return spisor_fail("the argument carries no usable places",
                       "send {\"places\": {\"state\": \"<absolute directory>\", \"cache\": \"<absolute directory>\"}}; "
                       "cache is needed only by forget",
                       out, out_len);
}

static const char *absolute_place(const cJSON *places, const char *name) {
    const char *place = string_member(places, name);
    return place != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place) ? place : NULL;
}

static int32_t serve(const uint8_t *in, size_t in_len, int needs_cache, registry_handler handler, uint8_t **out,
                     size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(call.argument, "places");
    registry_request request = {call.argument, absolute_place(places, "state"), absolute_place(places, "cache")};
    if (request.state == NULL || (needs_cache && request.cache == NULL)) {
        status = refuse_places(out, out_len);
    } else {
        status = handler(&request, out, out_len);
    }
    spisor_discard_call(&call);
    return status;
}

RUNTIME_EXPORT int32_t runtime_registry_register(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, 0, register_host, out, out_len);
}

int32_t runtime_registry_register_first_provenance(const uint8_t *in, size_t in_len, uint8_t **out,
                                                   size_t *out_len) {
    return serve(in, in_len, 0, register_host_keeping_provenance, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_registry_get(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, 0, get_host, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_registry_list(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, 0, list_hosts, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_registry_forget(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, 1, forget_host, out, out_len);
}

int32_t runtime_registry_forget_locked(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return serve(in, in_len, 1, forget_host_locked, out, out_len);
}
