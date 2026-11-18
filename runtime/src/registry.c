#include "registry.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "directory_tree.h"
#include "hosts_lock.h"
#include "file_presence.h"
#include "json_service.h"
#include "paths.h"
#include "segment.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#define REGISTRATION_SUFFIX ".json"
#define REGISTRATION_SUFFIX_LENGTH 5

typedef struct registry_request {
    const cJSON *argument;
    const char *state;
    const char *cache;
} registry_request;

typedef cJSON *(*registry_handler)(const registry_request *request, cJSON **refusal);

static cJSON *out_of_memory(cJSON **refusal) {
    *refusal = NULL;
    return NULL;
}

static cJSON *answer_with(cJSON *result, cJSON **refusal) {
    *refusal = NULL;
    return result;
}

static cJSON *fail(const char *message, const char *fix, cJSON **refusal) {
    *refusal = runtime_refusal(message, fix);
    return NULL;
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
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static int is_host_id(const char *id) {
    return runtime_is_portable_segment(id);
}

static char *registration_path(const char *state, const char *id) {
    char *hosts = join_two(state, "hosts");
    char *file_name = spisor_memory_allocate(strlen(id) + REGISTRATION_SUFFIX_LENGTH + 1);
    char *path = NULL;
    if (hosts != NULL && file_name != NULL) {
        snprintf(file_name, strlen(id) + REGISTRATION_SUFFIX_LENGTH + 1, "%s%s", id, REGISTRATION_SUFFIX);
        path = join_two(hosts, file_name);
    }
    spisor_memory_release(hosts);
    spisor_memory_release(file_name);
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
    spisor_memory_release(path);
    if (file_state == SPISOR_JSON_FILE_ABSENT) {
        return RUNTIME_REGISTRY_ABSENT;
    }
    if (file_state == SPISOR_JSON_FILE_OUT_OF_MEMORY) {
        return RUNTIME_REGISTRY_OUT_OF_MEMORY;
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
    char *terminated = spisor_memory_reallocate(text, text_length + 2);
    if (terminated == NULL) {
        spisor_memory_release(text);
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
    spisor_memory_release(hosts);
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
        spisor_memory_release(path);
        return RUNTIME_REGISTRY_OUT_OF_MEMORY;
    }
    int written = make_hosts_directory(state) && spisor_replace_file(path, bytes, length);
    spisor_memory_release(path);
    spisor_memory_release(bytes);
    return written ? RUNTIME_REGISTRY_OK : RUNTIME_REGISTRY_UNWRITABLE;
}

#define UNREADABLE_MESSAGE "the host's registration is not a readable JSON object for that id"

static void unreadable_fix(const char *state, const char *id, char *fix, size_t size) {
    char *path = registration_path(state, id);
    snprintf(fix, size, "repair %s so it holds the host's registration as one JSON object, or remove it; it was left unchanged",
             path != NULL ? path : "the registration file");
    spisor_memory_release(path);
}

static cJSON *refuse_unreadable(const char *state, const char *id, cJSON **refusal) {
    char fix[2048];
    unreadable_fix(state, id, fix, sizeof fix);
    return fail(UNREADABLE_MESSAGE, fix, refusal);
}

int runtime_registration_usable(const cJSON *registration, const char *id, const char *state, cJSON **refusal) {
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(registration, "version")) &&
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(registration, "home"))) {
        return 1;
    }
    const char *format = "the host %s is not registered in %s";
    size_t size = strlen(format) + strlen(id) + strlen(state) + 1;
    char *message = spisor_memory_allocate(size);
    if (message != NULL) {
        snprintf(message, size, format, id, state);
        *refusal = runtime_refusal(message, RUNTIME_REINSTALL_FIX);
    }
    spisor_memory_release(message);
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

static cJSON *refuse_id(cJSON **refusal) {
    return fail("the argument carries no usable host id",
                "send {\"id\": \"<host id>\"}, a portable path segment with no surrounding whitespace", refusal);
}

static cJSON *refuse_unwritable(const char *state, cJSON **refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts can be created and written", state);
    return fail("the host's registration could not be written", fix, refusal);
}

static cJSON *refuse_answer(runtime_registry_answer answer, const registry_request *request, const char *id,
                            cJSON **refusal) {
    switch (answer) {
    case RUNTIME_REGISTRY_UNREADABLE:
        return refuse_unreadable(request->state, id, refusal);
    case RUNTIME_REGISTRY_INVALID:
        return refuse_id(refusal);
    case RUNTIME_REGISTRY_UNWRITABLE:
        return refuse_unwritable(request->state, refusal);
    default:
        return out_of_memory(refusal);
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
    if (!runtime_is_usable_home(string_member(registration, "home"))) {
        return "the registration's home is missing, empty or not an absolute path";
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

static cJSON *refuse_registration(const char *defect, cJSON **refusal) {
    return fail(defect,
                "send {\"registration\": {\"id\", \"kind\": \"host\", \"version\", \"home\": <absolute path>, "
                "\"application\"?, "
                "\"provenance\": {\"method\", \"version\", \"location\": <absolute path>, \"os\", \"architecture\", "
                "\"installedAt\": <ISO 8601 UTC>, ...}, "
                "\"requiresCommand\", \"start\"?: {\"runtime\": \"javascript\" | \"jvm\" | \"native\", "
                "\"entry\": <relative path inside the bundle>}}}", refusal);
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

/**
 * Stores merged, which it takes, answering {registration}; the answer is built first, so memory running out writes
 * nothing.
 */
static cJSON *store_and_answer(const registry_request *request, cJSON *merged, cJSON **refusal) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL || !cJSON_AddItemToObject(result, "registration", merged)) {
        cJSON_Delete(result);
        cJSON_Delete(merged);
        return out_of_memory(refusal);
    }
    runtime_registry_answer stored = runtime_registry_store(request->state, merged);
    if (stored != RUNTIME_REGISTRY_OK) {
        cJSON *answer = refuse_answer(stored, request, string_member(merged, "id"), refusal);
        cJSON_Delete(result);
        return answer;
    }
    return answer_with(result, refusal);
}

static cJSON *refuse_unlocked(const char *state, cJSON **refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts/.lock can be created, opened and locked", state);
    return fail("the registry lock could not be taken; nothing was changed", fix, refusal);
}

static cJSON *register_locked(const registry_request *request, const cJSON *given, int keep_stored_provenance,
                              cJSON **refusal) {
    const char *id = string_member(given, "id");
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, refusal);
    }
    cJSON *merged = merged_registration(given, stored, keep_stored_provenance);
    cJSON_Delete(stored);
    if (merged == NULL) {
        return out_of_memory(refusal);
    }
    return store_and_answer(request, merged, refusal);
}

static cJSON *register_with(const registry_request *request, int keep_stored_provenance, cJSON **refusal) {
    const cJSON *given = cJSON_GetObjectItemCaseSensitive(request->argument, "registration");
    const char *defect = registration_defect(given);
    if (defect != NULL) {
        return refuse_registration(defect, refusal);
    }
    spisor_lock *guard;
    spisor_status locked = runtime_hosts_lock(request->state, &guard);
    if (locked != SPISOR_OK) {
        return locked == SPISOR_OUT_OF_MEMORY ? out_of_memory(refusal) : refuse_unlocked(request->state, refusal);
    }
    cJSON *answer = register_locked(request, given, keep_stored_provenance, refusal);
    spisor_unlock(guard);
    return answer;
}

static cJSON *register_host(const registry_request *request, cJSON **refusal) {
    return register_with(request, 0, refusal);
}

static cJSON *register_host_keeping_provenance(const registry_request *request, cJSON **refusal) {
    return register_with(request, 1, refusal);
}

static cJSON *get_host(const registry_request *request, cJSON **refusal) {
    const char *id = string_member(request->argument, "id");
    if (id == NULL) {
        return refuse_id(refusal);
    }
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, refusal);
    }
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && stored != NULL && !cJSON_AddItemToObject(result, "registration", stored)) {
        cJSON_Delete(result);
        result = NULL;
    }
    if (result == NULL) {
        cJSON_Delete(stored);
    }
    return answer_with(result, refusal);
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
            spisor_memory_release(id);
        }
    }
    return complete;
}

static cJSON *refuse_unlistable(const char *state, cJSON **refusal) {
    char fix[2048];
    snprintf(fix, sizeof fix, "check that %s/hosts is a readable directory", state);
    return fail("the registry directory could not be read", fix, refusal);
}

static cJSON *list_hosts(const registry_request *request, cJSON **refusal) {
    char *directory = join_two(request->state, "hosts");
    runtime_directory_listing listing;
    if (directory == NULL) {
        return out_of_memory(refusal);
    }
    spisor_status listed = runtime_list_directory(directory, &listing);
    spisor_memory_release(directory);
    if (listed != SPISOR_OK) {
        return listed == SPISOR_OUT_OF_MEMORY ? out_of_memory(refusal) : refuse_unlistable(request->state, refusal);
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
    return answer_with(result, refusal);
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

static cJSON *refuse_unreversed(const char *id, int count, cJSON **refusal) {
    char message[512];
    snprintf(message, sizeof message, "the host %s still holds %d change(s) that were not reversed", id, count);
    return fail(message, "reverse the host's recorded changes first; its registration was left in place", refusal);
}

static int remove_host_directory(const char *place, const char *id) {
    char *directory = join_three(place, "hosts", id);
    int removed = directory != NULL && runtime_remove_tree(directory);
    spisor_memory_release(directory);
    return removed;
}

static int remove_registration_file(const char *state, const char *id) {
    char *path = registration_path(state, id);
    int removed = path != NULL && runtime_remove_tree(path);
    spisor_memory_release(path);
    return removed;
}

static cJSON *refuse_incomplete_removal(const char *id, cJSON **refusal) {
    char message[512];
    snprintf(message, sizeof message, "the host %s could not be removed completely", id);
    return fail(message, "close whatever holds its files open and forget it again; the registration is deleted "
                "last, so nothing was orphaned", refusal);
}

/** {forgotten}, built before anything is removed, so memory running out removes nothing. */
static cJSON *forgotten_answer(int forgotten) {
    cJSON *result = cJSON_CreateObject();
    if (result != NULL && cJSON_AddBoolToObject(result, "forgotten", forgotten) == NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    return result;
}

static cJSON *forget_locked(const registry_request *request, const char *id, cJSON **refusal) {
    cJSON *stored = NULL;
    runtime_registry_answer loaded = runtime_registry_load(request->state, id, &stored);
    if (loaded != RUNTIME_REGISTRY_OK && loaded != RUNTIME_REGISTRY_ABSENT) {
        return refuse_answer(loaded, request, id, refusal);
    }
    int unreversed = count_unreversed(stored);
    cJSON_Delete(stored);
    if (unreversed > 0) {
        return refuse_unreversed(id, unreversed, refusal);
    }
    cJSON *result = forgotten_answer(loaded == RUNTIME_REGISTRY_OK);
    if (result == NULL) {
        return out_of_memory(refusal);
    }
    int removed = remove_host_directory(request->state, id) && remove_host_directory(request->cache, id) &&
                  remove_registration_file(request->state, id);
    if (!removed) {
        cJSON_Delete(result);
        return refuse_incomplete_removal(id, refusal);
    }
    return answer_with(result, refusal);
}

static cJSON *forget_host(const registry_request *request, cJSON **refusal) {
    const char *id = string_member(request->argument, "id");
    if (id == NULL) {
        return refuse_id(refusal);
    }
    spisor_lock *guard;
    spisor_status locked = runtime_hosts_lock(request->state, &guard);
    if (locked != SPISOR_OK) {
        return locked == SPISOR_OUT_OF_MEMORY ? out_of_memory(refusal) : refuse_unlocked(request->state, refusal);
    }
    cJSON *answer = forget_locked(request, id, refusal);
    spisor_unlock(guard);
    return answer;
}

static cJSON *forget_host_locked(const registry_request *request, cJSON **refusal) {
    const char *id = string_member(request->argument, "id");
    return id == NULL ? refuse_id(refusal) : forget_locked(request, id, refusal);
}

static cJSON *refuse_places(cJSON **refusal) {
    return fail("the argument carries no usable places",
                "send {\"places\": {\"state\": \"<absolute directory>\", \"cache\": \"<absolute directory>\"}}; "
                "cache is needed only by forget",
                refusal);
}

static const char *absolute_place(const cJSON *places, const char *name) {
    const char *place = string_member(places, name);
    return place != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, place) ? place : NULL;
}

static cJSON *serve(const cJSON *argument, int needs_cache, registry_handler handler, cJSON **refusal) {
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(argument, "places");
    registry_request request = {argument, absolute_place(places, "state"), absolute_place(places, "cache")};
    *refusal = NULL;
    if (request.state == NULL || (needs_cache && request.cache == NULL)) {
        return refuse_places(refusal);
    }
    return handler(&request, refusal);
}

cJSON *runtime_registry_register_host(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 0, register_host, refusal);
}

cJSON *runtime_registry_register_first_provenance(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 0, register_host_keeping_provenance, refusal);
}

cJSON *runtime_registry_get_host(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 0, get_host, refusal);
}

cJSON *runtime_registry_list_hosts(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 0, list_hosts, refusal);
}

cJSON *runtime_registry_forget_host(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 1, forget_host, refusal);
}

cJSON *runtime_registry_forget_locked(const cJSON *argument, cJSON **refusal) {
    return serve(argument, 1, forget_host_locked, refusal);
}

const char *runtime_registration_defect(const cJSON *registration) {
    return registration_defect(registration);
}

int runtime_registry_is_host_id(const char *id) {
    return id != NULL && is_host_id(id);
}
