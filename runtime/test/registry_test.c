#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <pthread.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "directory_tree.h"
#include "registry.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

typedef int32_t (*registry_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char state[1100];
    char cache[1100];
} scratch;

typedef struct answer {
    int32_t status;
    cJSON *tree;
} answer;

static void fresh_scratch(scratch *place, const char *label) {
    static int sequence = 0;
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/registry-%d-%d-%s",
             (int)CURRENT_PROCESS_ID, sequence++, label);
    snprintf(place->state, sizeof place->state, "%s/state", place->directory);
    snprintf(place->cache, sizeof place->cache, "%s/cache", place->directory);
    CHECK(spisor_make_directories(place->directory) == 1);
}

static cJSON *places_for(const scratch *place) {
    cJSON *places = cJSON_CreateObject();
    cJSON_AddStringToObject(places, "state", place->state);
    cJSON_AddStringToObject(places, "cache", place->cache);
    return places;
}

static cJSON *request_for(const scratch *place) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddItemToObject(request, "places", places_for(place));
    return request;
}

static cJSON *id_request(const scratch *place, const char *id) {
    cJSON *request = request_for(place);
    cJSON_AddStringToObject(request, "id", id);
    return request;
}

static cJSON *registration_for(const char *id, const char *version, const char *method, const char *location) {
    cJSON *registration = cJSON_CreateObject();
    cJSON_AddStringToObject(registration, "id", id);
    cJSON_AddStringToObject(registration, "kind", "host");
    cJSON_AddStringToObject(registration, "version", version);
    cJSON_AddStringToObject(registration, "home", "/home/claude");
    cJSON *provenance = cJSON_AddObjectToObject(registration, "provenance");
    cJSON_AddStringToObject(provenance, "method", method);
    cJSON_AddStringToObject(provenance, "version", version);
    cJSON_AddStringToObject(provenance, "location", location);
    cJSON_AddStringToObject(provenance, "os", "linux");
    cJSON_AddStringToObject(provenance, "architecture", "x64");
    cJSON_AddStringToObject(provenance, "installedAt", "2026-11-02T10:00:00Z");
    cJSON_AddBoolToObject(registration, "requiresCommand", 1);
    return registration;
}

static cJSON *register_request(const scratch *place, cJSON *registration) {
    cJSON *request = request_for(place);
    cJSON_AddItemToObject(request, "registration", registration);
    return request;
}

static answer call(registry_export function, cJSON *request) {
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    uint8_t *out = NULL;
    size_t out_len = 0;
    answer result;
    result.status = function((const uint8_t *)text, strlen(text), &out, &out_len);
    result.tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
    free(text);
    return result;
}

static int is_refusal(const answer *result) {
    return result->status == SPISOR_STATUS_FAILURE &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(result->tree, "message"));
}

static int refuses(registry_export function, cJSON *request) {
    answer result = call(function, request);
    int refused = is_refusal(&result);
    cJSON_Delete(result.tree);
    return refused;
}

static int refusal_names(registry_export function, cJSON *request, const char *text) {
    answer result = call(function, request);
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "message"));
    const char *fix = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "fix"));
    int names = is_refusal(&result) && ((message != NULL && strstr(message, text) != NULL) ||
                                        (fix != NULL && strstr(fix, text) != NULL));
    if (!names) {
        fprintf(stderr, "expected a refusal naming \"%s\", got status %d\n", text, (int)result.status);
    }
    cJSON_Delete(result.tree);
    return names;
}

static int register_host(const scratch *place, cJSON *registration) {
    answer result = call(runtime_registry_register, register_request(place, registration));
    int succeeded = result.status == SPISOR_STATUS_SUCCESS;
    cJSON_Delete(result.tree);
    return succeeded;
}

/** The registration get answers, freed with cJSON_Delete, or NULL when it answers none. */
static cJSON *get_registration(const scratch *place, const char *id) {
    answer result = call(runtime_registry_get, id_request(place, id));
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    cJSON *registration = cJSON_DetachItemFromObjectCaseSensitive(result.tree, "registration");
    cJSON_Delete(result.tree);
    return registration;
}

static void write_raw_file(const char *path, const char *content) {
    CHECK(spisor_replace_file(path, (const uint8_t *)content, strlen(content)) == 1);
}

static void make_host_file(const scratch *place, const char *id, const char *relative_file, int in_cache) {
    char directory[1400];
    char path[1600];
    snprintf(directory, sizeof directory, "%s/hosts/%s", in_cache ? place->cache : place->state, id);
    snprintf(path, sizeof path, "%s/%s", directory, relative_file);
    CHECK(spisor_make_directories(directory) == 1);
    write_raw_file(path, "kept");
}

static int host_file_exists(const scratch *place, const char *id) {
    char path[1400];
    snprintf(path, sizeof path, "%s/hosts/%s.json", place->state, id);
    return spisor_file_exists(path);
}

static int host_directory_exists(const char *place, const char *id) {
    char path[1400];
    snprintf(path, sizeof path, "%s/hosts/%s", place, id);
    return spisor_file_exists(path);
}

static void store_directly(const scratch *place, cJSON *registration) {
    CHECK(runtime_registry_store(place->state, registration) == RUNTIME_REGISTRY_OK);
    cJSON_Delete(registration);
}

static cJSON *loaded_directly(const scratch *place, const char *id) {
    cJSON *registration = NULL;
    CHECK(runtime_registry_load(place->state, id, &registration) == RUNTIME_REGISTRY_OK);
    return registration;
}

static const char *text_at(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static const char *provenance_text(const cJSON *registration, const char *member) {
    return text_at(cJSON_GetObjectItemCaseSensitive(registration, "provenance"), member);
}

static int list_count(const scratch *place) {
    answer result = call(runtime_registry_list, request_for(place));
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(result.tree, "hosts");
    int count = result.status == SPISOR_STATUS_SUCCESS && cJSON_IsArray(hosts) ? cJSON_GetArraySize(hosts) : -1;
    cJSON_Delete(result.tree);
    return count;
}

static void register_then_get(void) {
    scratch place;
    fresh_scratch(&place, "get");
    answer registered = call(runtime_registry_register,
                             register_request(&place, registration_for("claude", "1.0.0", "npm", place.directory)));
    CHECK(registered.status == SPISOR_STATUS_SUCCESS);
    const cJSON *answered = cJSON_GetObjectItemCaseSensitive(registered.tree, "registration");
    check_text(text_at(answered, "id"), "claude", "registered id");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answered, "commands")) == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(answered, "changes")) == 0);
    cJSON_Delete(registered.tree);

    cJSON *fetched = get_registration(&place, "claude");
    check_text(text_at(fetched, "kind"), "host", "kind");
    check_text(text_at(fetched, "version"), "1.0.0", "version");
    check_text(text_at(fetched, "home"), "/home/claude", "home");
    check_text(provenance_text(fetched, "method"), "npm", "method");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(fetched, "requiresCommand")));
    CHECK(host_file_exists(&place, "claude"));
    cJSON_Delete(fetched);
    CHECK(get_registration(&place, "unknown") == NULL);
}

static void add_command_and_change(const scratch *place, const char *id) {
    cJSON *stored = loaded_directly(place, id);
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "name", "claude-weivel");
    cJSON_AddStringToObject(command, "requested", "claude-weivel");
    cJSON_AddStringToObject(command, "owner", "host");
    cJSON_AddStringToObject(command, "state", "active");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(stored, "commands"), command);
    cJSON *change = cJSON_CreateObject();
    cJSON_AddNumberToObject(change, "sequence", 1);
    cJSON_AddStringToObject(change, "kind", "file");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(stored, "changes"), change);
    store_directly(place, stored);
}

static void re_registration_keeps_commands_and_changes_and_replaces_the_rest(void) {
    scratch place;
    fresh_scratch(&place, "reregister");
    CHECK(register_host(&place, registration_for("claude", "1.0.0", "npm", place.directory)));
    add_command_and_change(&place, "claude");

    cJSON *newer = registration_for("claude", "2.0.0", "winget", place.cache);
    cJSON_DeleteItemFromObjectCaseSensitive(newer, "home");
    cJSON_AddItemToObject(newer, "commands", cJSON_CreateArray());
    cJSON_AddItemToObject(newer, "changes", cJSON_CreateArray());
    CHECK(register_host(&place, newer));

    cJSON *fetched = get_registration(&place, "claude");
    check_text(text_at(fetched, "version"), "2.0.0", "version replaced");
    check_text(provenance_text(fetched, "method"), "winget", "provenance replaced");
    check_text(provenance_text(fetched, "location"), place.cache, "location replaced");
    CHECK(cJSON_GetObjectItemCaseSensitive(fetched, "home") == NULL);
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(fetched, "commands");
    check_text(text_at(cJSON_GetArrayItem(commands, 0), "name"), "claude-weivel", "command kept");
    CHECK(cJSON_GetArraySize(commands) == 1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(fetched, "changes")) == 1);
    cJSON_Delete(fetched);
}

typedef struct registering {
    const scratch *place;
    const char *id;
    int succeeded;
} registering;

#ifdef _WIN32
typedef HANDLE thread_handle;

static DWORD WINAPI registering_entry(LPVOID argument) {
    registering *work = argument;
    work->succeeded = register_host(work->place, registration_for(work->id, "1.0.0", "npm", work->place->directory));
    return 0;
}

static void run_two(registering *first, registering *second) {
    thread_handle handles[2];
    handles[0] = CreateThread(NULL, 0, registering_entry, first, 0, NULL);
    handles[1] = CreateThread(NULL, 0, registering_entry, second, 0, NULL);
    CHECK(handles[0] != NULL && handles[1] != NULL);
    for (int index = 0; index < 2; index++) {
        if (handles[index] != NULL) {
            WaitForSingleObject(handles[index], INFINITE);
            CloseHandle(handles[index]);
        }
    }
}
#else
static void *registering_entry(void *argument) {
    registering *work = argument;
    work->succeeded = register_host(work->place, registration_for(work->id, "1.0.0", "npm", work->place->directory));
    return NULL;
}

static void run_two(registering *first, registering *second) {
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, registering_entry, first) == 0);
    CHECK(pthread_create(&threads[1], NULL, registering_entry, second) == 0);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
}
#endif

static void two_threads_registering_do_not_lose_either_host(void) {
    for (int round = 0; round < 40; round++) {
        scratch place;
        fresh_scratch(&place, "threads");
        registering first = {&place, "alpha", 0};
        registering second = {&place, "beta", 0};
        run_two(&first, &second);
        CHECK(first.succeeded && second.succeeded);
        CHECK(list_count(&place) == 2);
    }
}

static void list_sorts_hosts_and_marks_a_missing_location_as_orphan(void) {
    scratch place;
    fresh_scratch(&place, "orphan");
    char location[1200];
    snprintf(location, sizeof location, "%s/binary", place.directory);
    CHECK(spisor_make_directories(location) == 1);
    CHECK(register_host(&place, registration_for("zulu", "1.0.0", "npm", location)));
    CHECK(register_host(&place, registration_for("alpha", "1.0.0", "npm", location)));

    answer present = call(runtime_registry_list, request_for(&place));
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(present.tree, "hosts");
    CHECK(cJSON_GetArraySize(hosts) == 2);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 0), "registration"), "id"), "alpha",
               "first id");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 0), "orphan")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 1), "orphan")));
    cJSON_Delete(present.tree);

    CHECK(runtime_remove_tree(location) == 1);
    answer gone = call(runtime_registry_list, request_for(&place));
    hosts = cJSON_GetObjectItemCaseSensitive(gone.tree, "hosts");
    CHECK(cJSON_GetArraySize(hosts) == 2);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 0), "orphan")));
    cJSON_Delete(gone.tree);
}

static void list_with_no_registry_is_empty(void) {
    scratch place;
    fresh_scratch(&place, "empty");
    CHECK(list_count(&place) == 0);
}

static void a_host_named_like_a_registration_file_lists_once(void) {
    scratch place;
    fresh_scratch(&place, "dotted");
    CHECK(register_host(&place, registration_for("a.json", "1.0.0", "npm", place.directory)));
    make_host_file(&place, "a.json", "own.txt", 0);
    answer result = call(runtime_registry_list, request_for(&place));
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(result.tree, "hosts");
    CHECK(cJSON_GetArraySize(hosts) == 1);
    check_text(text_at(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hosts, 0), "registration"), "id"), "a.json",
               "dotted id");
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(result.tree, "unreadable")) == 0);
    cJSON_Delete(result.tree);
}

static void mark_every_change_reversed(const scratch *place, const char *id) {
    cJSON *stored = loaded_directly(place, id);
    const cJSON *change;
    cJSON_ArrayForEach(change, cJSON_GetObjectItemCaseSensitive(stored, "changes")) {
        cJSON_AddBoolToObject((cJSON *)change, "reversed", 1);
    }
    store_directly(place, stored);
}

static void forget_is_refused_while_a_change_is_unreversed_then_removes_everything(void) {
    scratch place;
    fresh_scratch(&place, "forget");
    CHECK(register_host(&place, registration_for("claude", "1.0.0", "npm", place.directory)));
    CHECK(register_host(&place, registration_for("other", "1.0.0", "npm", place.directory)));
    add_command_and_change(&place, "claude");
    make_host_file(&place, "claude", "nested.txt", 0);
    make_host_file(&place, "claude", "downloads.bin", 1);
    make_host_file(&place, "other", "own.txt", 0);

    CHECK(refusal_names(runtime_registry_forget, id_request(&place, "claude"), "not reversed"));
    CHECK(host_file_exists(&place, "claude"));
    CHECK(host_directory_exists(place.state, "claude"));
    CHECK(host_directory_exists(place.cache, "claude"));

    mark_every_change_reversed(&place, "claude");
    answer forgotten = call(runtime_registry_forget, id_request(&place, "claude"));
    CHECK(forgotten.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(forgotten.tree, "forgotten")));
    cJSON_Delete(forgotten.tree);
    CHECK(!host_file_exists(&place, "claude"));
    CHECK(!host_directory_exists(place.state, "claude"));
    CHECK(!host_directory_exists(place.cache, "claude"));
    CHECK(get_registration(&place, "claude") == NULL);
    CHECK(host_file_exists(&place, "other") && host_directory_exists(place.state, "other"));
}

static void forgetting_an_unknown_host_succeeds_and_says_nothing_was_there(void) {
    scratch place;
    fresh_scratch(&place, "forget-unknown");
    answer result = call(runtime_registry_forget, id_request(&place, "ghost"));
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result.tree, "forgotten")));
    cJSON_Delete(result.tree);
}

static void a_bad_id_is_refused_everywhere(void) {
    static const char *const bad_ids[] = {"../x", "a/b", "a\\b", "", " a", "a ", "CON", "..", "a:b"};
    scratch place;
    fresh_scratch(&place, "badid");
    char sentinel[1200];
    snprintf(sentinel, sizeof sentinel, "%s/sentinel.txt", place.state);
    CHECK(spisor_make_directories(place.state) == 1);
    write_raw_file(sentinel, "kept");
    for (size_t index = 0; index < sizeof bad_ids / sizeof bad_ids[0]; index++) {
        const char *id = bad_ids[index];
        CHECK(refuses(runtime_registry_register, register_request(&place, registration_for(id, "1", "npm", "/x"))));
        CHECK(refuses(runtime_registry_get, id_request(&place, id)));
        CHECK(refuses(runtime_registry_forget, id_request(&place, id)));
    }
    CHECK(spisor_file_exists(sentinel));
    CHECK(list_count(&place) == 0);
}

static void an_unusable_registration_is_refused(void) {
    scratch place;
    fresh_scratch(&place, "badshape");
    cJSON *wrong_kind = registration_for("h", "1", "npm", "/x");
    cJSON_ReplaceItemInObjectCaseSensitive(wrong_kind, "kind", cJSON_CreateString("plugin"));
    CHECK(refuses(runtime_registry_register, register_request(&place, wrong_kind)));
    cJSON *no_provenance = registration_for("h", "1", "npm", "/x");
    cJSON_DeleteItemFromObjectCaseSensitive(no_provenance, "provenance");
    CHECK(refuses(runtime_registry_register, register_request(&place, no_provenance)));
    cJSON *no_requirement = registration_for("h", "1", "npm", "/x");
    cJSON_DeleteItemFromObjectCaseSensitive(no_requirement, "requiresCommand");
    CHECK(refuses(runtime_registry_register, register_request(&place, no_requirement)));
    CHECK(refuses(runtime_registry_register, request_for(&place)));
    CHECK(!host_file_exists(&place, "h"));
}

static void unusable_places_are_refused(void) {
    scratch place;
    fresh_scratch(&place, "places");
    cJSON *relative = register_request(&place, registration_for("h", "1", "npm", "/x"));
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(relative, "places"), "state",
                                           cJSON_CreateString("relative/state"));
    CHECK(refuses(runtime_registry_register, relative));
    cJSON *no_cache = id_request(&place, "h");
    cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(no_cache, "places"), "cache");
    CHECK(refuses(runtime_registry_forget, no_cache));
    CHECK(refuses(runtime_registry_list, cJSON_CreateObject()));
}

static void expect_file_bytes(const char *path, const char *expected, const char *label) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    check_text((const char *)bytes, expected, label);
    free(bytes);
}

static void an_unreadable_registration_is_reported_and_never_overwritten(void) {
    scratch place;
    fresh_scratch(&place, "unreadable");
    CHECK(register_host(&place, registration_for("fine", "1.0.0", "npm", place.directory)));
    char path[1300];
    snprintf(path, sizeof path, "%s/hosts/broken.json", place.state);
    write_raw_file(path, "{ not json");

    CHECK(refusal_names(runtime_registry_register,
                        register_request(&place, registration_for("broken", "1.0.0", "npm", "/x")), "broken.json"));
    expect_file_bytes(path, "{ not json", "register left the file");
    CHECK(refuses(runtime_registry_get, id_request(&place, "broken")));
    CHECK(refuses(runtime_registry_forget, id_request(&place, "broken")));
    expect_file_bytes(path, "{ not json", "get and forget left the file");

    answer listed = call(runtime_registry_list, request_for(&place));
    CHECK(listed.status == SPISOR_STATUS_SUCCESS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(listed.tree, "hosts")) == 1);
    const cJSON *unreadable = cJSON_GetObjectItemCaseSensitive(listed.tree, "unreadable");
    CHECK(cJSON_GetArraySize(unreadable) == 1);
    check_text(cJSON_GetStringValue(cJSON_GetArrayItem(unreadable, 0)), "broken", "unreadable id");
    cJSON_Delete(listed.tree);

    write_raw_file(path, "{\"id\": \"someone-else\"}");
    CHECK(refuses(runtime_registry_get, id_request(&place, "broken")));
}

int main(void) {
    register_then_get();
    re_registration_keeps_commands_and_changes_and_replaces_the_rest();
    two_threads_registering_do_not_lose_either_host();
    list_sorts_hosts_and_marks_a_missing_location_as_orphan();
    list_with_no_registry_is_empty();
    a_host_named_like_a_registration_file_lists_once();
    forget_is_refused_while_a_change_is_unreversed_then_removes_everything();
    forgetting_an_unknown_host_succeeds_and_says_nothing_was_there();
    a_bad_id_is_refused_everywhere();
    an_unusable_registration_is_refused();
    unusable_places_are_refused();
    an_unreadable_registration_is_reported_and_never_overwritten();
    return check_summary("registry_test");
}
