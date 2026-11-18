#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define CURRENT_PROCESS_ID _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "spisor/allocation.h"
#include "definition_fixture.h"
#include "scratch_directory.h"
#include "fake_secret_backend.h"
#include "links.h"
#include "runtime/secrets.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_kind.h"

typedef enum secrets_operation {
    SECRETS_GET,
    SECRETS_SET,
    SECRETS_REMOVE,
    SECRETS_CLEAR,
    SECRETS_RECONCILE
} secrets_operation;

typedef struct scratch {
    char directory[1024];
    char file[1100];
    int force_file_store;
} scratch;

#define VALUE_CAP_BYTES 2560000u

/** One call's arguments; `place` NULL sends no places at all, `relative_file` a secrets file that is not absolute. */
typedef struct request {
    const scratch *place;
    const char *relative_file;
    const char *plugin;
    const char *name;
    const char *value;
} request;

/** What one call answered, copied out of the definition's answer and refusal before both were released. */
typedef struct answer {
    spisor_status status;
    char *value;
    uint32_t location;
    uint64_t moved;
    char *detail;
    char *fix;
} answer;

static void fresh_scratch(scratch *place, const char *label) {
    static int sequence = 0;
    snprintf(place->directory, sizeof place->directory, RUNTIME_SCRATCH_PATH "/secrets-%d-%d-%s",
             (int)CURRENT_PROCESS_ID, sequence++, label);
    snprintf(place->file, sizeof place->file, "%s/state/secrets.json", place->directory);
    place->force_file_store = 0;
    start_empty_directory(place->directory);
    fake_store_reset();
}

static request request_for(const scratch *place, const char *plugin, const char *name, const char *value) {
    request built = {place, NULL, plugin, name, value};
    return built;
}

static char *copy_of(const char *text) {
    char *copy = text == NULL ? NULL : spisor_memory_allocate(strlen(text) + 1);
    if (copy != NULL) {
        memcpy(copy, text, strlen(text) + 1);
    }
    return copy;
}

static const runtime_secrets_definition *secrets_definition(void) {
    return runtime_secrets_definition_of();
}

static spisor_status run(secrets_operation operation, const runtime_secret_places *places, const request *call,
                         answer *result, spisor_refusal *refusal) {
    const runtime_secrets_definition *secrets = secrets_definition();
    runtime_secrets_entry_argument entry = SPISOR_INIT(runtime_secrets_entry_argument);
    runtime_secrets_set_argument set = SPISOR_INIT(runtime_secrets_set_argument);
    runtime_secrets_clear_argument clear = SPISOR_INIT(runtime_secrets_clear_argument);
    runtime_secrets_reconcile_argument reconcile = SPISOR_INIT(runtime_secrets_reconcile_argument);
    runtime_secrets_get_answer secret = SPISOR_INIT(runtime_secrets_get_answer);
    runtime_secrets_set_answer stored = SPISOR_INIT(runtime_secrets_set_answer);
    runtime_secrets_reconcile_answer reconciled = SPISOR_INIT(runtime_secrets_reconcile_answer);
    spisor_status status;
    entry.places = set.places = clear.places = reconcile.places = places;
    entry.plugin = set.plugin = clear.plugin = call->plugin;
    entry.name = set.name = call->name;
    set.value = call->value;
    switch (operation) {
    case SECRETS_GET:
        status = secrets->get(secrets->context, &entry, &secret, refusal);
        CHECK(secret.value == NULL || secret.length == strlen(secret.value));
        result->value = copy_of(secret.value);
        secrets->release_get_answer(secrets->context, &secret);
        CHECK(secret.value == NULL && secret.storage == NULL);
        return status;
    case SECRETS_SET:
        status = secrets->set(secrets->context, &set, &stored, refusal);
        result->location = stored.location;
        return status;
    case SECRETS_REMOVE:
        return secrets->remove(secrets->context, &entry, refusal);
    case SECRETS_CLEAR:
        return secrets->clear(secrets->context, &clear, refusal);
    default:
        status = secrets->reconcile(secrets->context, &reconcile, &reconciled, refusal);
        result->moved = reconciled.moved;
        return status;
    }
}

static answer call(secrets_operation operation, request call) {
    runtime_secret_places places = SPISOR_INIT(runtime_secret_places);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    answer result = {SPISOR_OK, NULL, 0, 0, NULL, NULL};
    if (call.place != NULL) {
        places.secrets_file = call.relative_file != NULL ? call.relative_file : call.place->file;
        places.force_file_store = (uint32_t)call.place->force_file_store;
    }
    result.status = run(operation, call.place == NULL ? NULL : &places, &call, &result, &refusal);
    result.detail = copy_of(refusal.detail);
    result.fix = copy_of(refusal.fix);
    secrets_definition()->release_refusal(secrets_definition()->context, &refusal);
    CHECK(refusal.storage == NULL);
    return result;
}

static void forget(answer *result) {
    spisor_memory_release(result->value);
    spisor_memory_release(result->detail);
    spisor_memory_release(result->fix);
}

static int succeeds(secrets_operation operation, request request) {
    answer result = call(operation, request);
    int succeeded = result.status == SPISOR_OK && result.detail == NULL;
    forget(&result);
    return succeeded;
}

/** Whether the call answered `expected` with a refusal carrying a detail and a fix. */
static int refuses_as(secrets_operation operation, request request, spisor_status expected) {
    answer result = call(operation, request);
    int refused = result.status == expected && result.detail != NULL && result.fix != NULL;
    if (!refused) {
        fprintf(stderr, "expected status %d with a refusal, got status %d and \"%s\"\n", (int)expected,
                (int)result.status, result.detail == NULL ? "(none)" : result.detail);
    }
    forget(&result);
    return refused;
}

static int refuses(secrets_operation operation, request request) {
    return refuses_as(operation, request, SPISOR_REFUSED);
}

static int malformed(secrets_operation operation, request request) {
    return refuses_as(operation, request, SPISOR_MALFORMED);
}

/** Whether the call was refused with `text` in its detail ("message") or its fix ("fix"). */
static int refuses_naming(secrets_operation operation, request request, const char *member, const char *text) {
    answer result = call(operation, request);
    const char *said = strcmp(member, "fix") == 0 ? result.fix : result.detail;
    int refused = (result.status == SPISOR_REFUSED || result.status == SPISOR_MALFORMED) && said != NULL &&
                  strstr(said, text) != NULL;
    if (!refused) {
        fprintf(stderr, "expected a refusal whose %s names \"%s\", got status %d and \"%s\"\n", member, text,
                (int)result.status, said == NULL ? "(none)" : said);
    }
    forget(&result);
    return refused;
}

static void expect_file_bytes(const char *path, const char *expected, const char *label) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    check_text((const char *)bytes, expected, label);
    spisor_memory_release(bytes);
}

static char *repeated_value(size_t length) {
    char *value = spisor_memory_allocate(length + 1);
    if (value != NULL) {
        memset(value, 'x', length);
        value[length] = '\0';
    }
    return value;
}

static void set_secret(const scratch *place, const char *plugin, const char *name, const char *value,
                       const char *expected_place) {
    answer result = call(SECRETS_SET, request_for(place, plugin, name, value));
    uint32_t expected = strcmp(expected_place, "store") == 0 ? RUNTIME_SECRET_LOCATION_STORE
                                                             : RUNTIME_SECRET_LOCATION_FILE;
    CHECK(result.status == SPISOR_OK);
    if (result.location != expected) {
        fprintf(stderr, "%s: expected the secret in the %s, got location %u\n", name, expected_place,
                (unsigned)result.location);
        check_failures++;
    }
    forget(&result);
}

/** The value get answers, as a copy freed, or NULL when it answers none. */
static char *get_secret(const scratch *place, const char *plugin, const char *name) {
    answer result = call(SECRETS_GET, request_for(place, plugin, name, NULL));
    char *value = result.value;
    CHECK(result.status == (value == NULL ? SPISOR_NONE : SPISOR_OK));
    result.value = NULL;
    forget(&result);
    return value;
}

static int moved_count(answer result) {
    int count = result.status == SPISOR_OK ? (int)result.moved : -1;
    forget(&result);
    return count;
}

static void expect_get(const scratch *place, const char *plugin, const char *name, const char *expected) {
    char *value = get_secret(place, plugin, name);
    if (expected == NULL) {
        CHECK(value == NULL);
    } else {
        check_text(value, expected, name);
    }
    spisor_memory_release(value);
}

static cJSON *file_entries(const scratch *place) {
    spisor_json_file_state state;
    cJSON *entries = spisor_read_json_file(place->file, &state);
    return state == SPISOR_JSON_FILE_ABSENT ? cJSON_CreateObject() : entries;
}

static int file_entry_count(const scratch *place) {
    cJSON *entries = file_entries(place);
    int count = cJSON_IsObject(entries) ? cJSON_GetArraySize(entries) : -1;
    cJSON_Delete(entries);
    return count;
}

static int file_holds(const scratch *place, const char *entry, const char *value) {
    cJSON *entries = file_entries(place);
    const char *held = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entries, entry));
    int holds = held != NULL && (value == NULL || strcmp(held, value) == 0);
    cJSON_Delete(entries);
    return holds;
}

static int store_holds(const char *entry, const char *value) {
    const char *held = fake_store_value(entry);
    return held != NULL && (value == NULL || strcmp(held, value) == 0);
}

static void write_raw_file(const scratch *place, const char *content) {
    char directory[1100];
    snprintf(directory, sizeof directory, "%s/state", place->directory);
    CHECK(spisor_make_directories(directory) == 1);
    CHECK(spisor_replace_file(place->file, (const uint8_t *)content, strlen(content)) == SPISOR_OK);
}

static void a_declined_set_lands_in_the_file(void) {
    scratch place;
    fresh_scratch(&place, "declined");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "v1", "file");
    CHECK(file_holds(&place, "spisor/github/token", "v1"));
    CHECK(fake_store_entry_count() == 0);
    expect_get(&place, "github", "token", "v1");
}

static void an_unavailable_store_sends_the_set_to_the_file(void) {
    scratch place;
    fresh_scratch(&place, "unavailable");
    fake_store_behave(FAKE_STORE_UNAVAILABLE);
    set_secret(&place, "github", "token", "v1", "file");
    CHECK(file_holds(&place, "spisor/github/token", "v1"));
    CHECK(fake_store_entry_count() == 0);
}

static void a_failing_store_fails_the_set_and_writes_nothing(void) {
    scratch place;
    fresh_scratch(&place, "failing");
    fake_store_behave(FAKE_STORE_FAILS);
    CHECK(refuses(SECRETS_SET, request_for(&place, "github", "token", "v1")));
    CHECK(spisor_probe_file(place.file) != SPISOR_FILE_PRESENT);
}

static void a_stored_set_moves_every_file_entry_and_empties_the_file(void) {
    scratch place;
    fresh_scratch(&place, "move");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "one", "1", "file");
    set_secret(&place, "github", "two", "2", "file");
    set_secret(&place, "gitlab", "three", "3", "file");
    CHECK(file_entry_count(&place) == 3);
    fake_store_behave(FAKE_STORE_ACCEPTS);
    set_secret(&place, "github", "four", "4", "store");
    CHECK(store_holds("spisor/github/one", "1"));
    CHECK(store_holds("spisor/github/two", "2"));
    CHECK(store_holds("spisor/gitlab/three", "3"));
    CHECK(store_holds("spisor/github/four", "4"));
    CHECK(file_entry_count(&place) == 0);
}

static void a_stored_set_is_not_overwritten_by_its_older_file_copy(void) {
    scratch place;
    fresh_scratch(&place, "older-copy");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "old", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    set_secret(&place, "github", "token", "new", "store");
    CHECK(store_holds("spisor/github/token", "new"));
    CHECK(file_entry_count(&place) == 0);
    expect_get(&place, "github", "token", "new");
}

static void in_exactly_one_place(const scratch *place, const char *entry, const char *value, int *in_store) {
    int stored = store_holds(entry, value);
    int filed = file_holds(place, entry, value);
    CHECK(stored != filed);
    *in_store += stored;
}

static void a_move_failing_partway_loses_nothing_and_duplicates_nothing(void) {
    scratch place;
    fresh_scratch(&place, "partial");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "one", "1", "file");
    set_secret(&place, "github", "two", "2", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    fake_store_fail_sets_after(2);
    set_secret(&place, "github", "three", "3", "store");
    CHECK(store_holds("spisor/github/three", "3"));
    int moved = 0;
    in_exactly_one_place(&place, "spisor/github/one", "1", &moved);
    in_exactly_one_place(&place, "spisor/github/two", "2", &moved);
    CHECK(moved == 1);
    CHECK(file_entry_count(&place) == 1);
}

static void get_prefers_the_file_and_never_writes_to_the_store(void) {
    scratch place;
    fresh_scratch(&place, "prefers");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "from-file", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    fake_store_seed("spisor/github/token", "from-store");
    fake_store_seed("spisor/github/only-in-store", "store-only");
    int sets_before = fake_store_set_calls();
    int calls_before = fake_store_calls();
    expect_get(&place, "github", "token", "from-file");
    CHECK(fake_store_calls() == calls_before);
    expect_get(&place, "github", "only-in-store", "store-only");
    expect_get(&place, "github", "missing", NULL);
    CHECK(fake_store_set_calls() == sets_before);
    CHECK(file_entry_count(&place) == 1);
}

static void a_value_set_while_the_store_declines_is_not_shadowed_by_its_stored_older_value(void) {
    scratch place;
    fresh_scratch(&place, "shadow");
    set_secret(&place, "github", "token", "A", "store");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "B", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    expect_get(&place, "github", "token", "B");
    set_secret(&place, "gitlab", "other", "x", "store");
    CHECK(store_holds("spisor/github/token", "B"));
    CHECK(file_entry_count(&place) == 0);
    expect_get(&place, "github", "token", "B");
}

static void get_fails_when_neither_place_holds_it_and_the_store_refused(void) {
    scratch place;
    fresh_scratch(&place, "get-refused");
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses(SECRETS_GET, request_for(&place, "github", "token", NULL)));
    fake_store_behave(FAKE_STORE_FAILS);
    CHECK(refuses(SECRETS_GET, request_for(&place, "github", "token", NULL)));
    fake_store_behave(FAKE_STORE_UNAVAILABLE);
    expect_get(&place, "github", "token", NULL);
}

static void delete_removes_from_both_places(void) {
    scratch place;
    fresh_scratch(&place, "delete");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "from-file", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    fake_store_seed("spisor/github/token", "from-store");
    CHECK(succeeds(SECRETS_REMOVE, request_for(&place, "github", "token", NULL)));
    CHECK(fake_store_value("spisor/github/token") == NULL);
    CHECK(!file_holds(&place, "spisor/github/token", NULL));
    expect_get(&place, "github", "token", NULL);
    CHECK(succeeds(SECRETS_REMOVE, request_for(&place, "github", "token", NULL)));
}

static void clear_removes_one_plugin_from_both_places_and_keeps_another(void) {
    scratch place;
    fresh_scratch(&place, "clear");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "one", "1", "file");
    set_secret(&place, "githubx", "one", "x", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    fake_store_seed("spisor/github/two", "2");
    fake_store_seed("spisor/gitlab/two", "g");
    CHECK(succeeds(SECRETS_CLEAR, request_for(&place, "github", NULL, NULL)));
    CHECK(fake_store_value("spisor/github/two") == NULL);
    CHECK(!file_holds(&place, "spisor/github/one", NULL));
    CHECK(store_holds("spisor/gitlab/two", "g"));
    CHECK(file_holds(&place, "spisor/githubx/one", "x"));
}

static void a_forced_file_store_never_touches_the_store(void) {
    scratch place;
    fresh_scratch(&place, "forced");
    place.force_file_store = 1;
    set_secret(&place, "github", "token", "v1", "file");
    expect_get(&place, "github", "token", "v1");
    CHECK(succeeds(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL)));
    CHECK(succeeds(SECRETS_REMOVE, request_for(&place, "github", "token", NULL)));
    set_secret(&place, "github", "other", "v2", "file");
    CHECK(succeeds(SECRETS_CLEAR, request_for(&place, "github", NULL, NULL)));
    CHECK(fake_store_calls() == 0);
    CHECK(file_entry_count(&place) == 0);
}

static void reconcile_moves_file_entries_once_the_store_appears(void) {
    scratch place;
    fresh_scratch(&place, "reconcile");
    fake_store_behave(FAKE_STORE_UNAVAILABLE);
    set_secret(&place, "github", "one", "1", "file");
    set_secret(&place, "gitlab", "two", "2", "file");
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 0);
    CHECK(file_entry_count(&place) == 2);
    fake_store_behave(FAKE_STORE_ACCEPTS);
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 2);
    CHECK(store_holds("spisor/github/one", "1"));
    CHECK(store_holds("spisor/gitlab/two", "2"));
    CHECK(file_entry_count(&place) == 0);
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 0);
}

static void reconcile_with_no_file_moves_nothing(void) {
    scratch place;
    fresh_scratch(&place, "reconcile-empty");
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 0);
    CHECK(spisor_probe_file(place.file) != SPISOR_FILE_PRESENT);
}

static char *long_utf8_value(size_t repeats) {
    const char *unit = "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x94\x91 ";
    size_t unit_length = strlen(unit);
    char *value = spisor_memory_allocate(unit_length * repeats + 1);
    for (size_t index = 0; value != NULL && index < repeats; index++) {
        memcpy(value + index * unit_length, unit, unit_length);
    }
    if (value != NULL) {
        value[unit_length * repeats] = '\0';
    }
    return value;
}

static void a_long_utf8_value_round_trips_through_the_file_and_the_move(void) {
    scratch place;
    fresh_scratch(&place, "long");
    char *value = long_utf8_value(1024);
    CHECK(value != NULL && strlen(value) > 10000);
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", value, "file");
    expect_get(&place, "github", "token", value);
    fake_store_behave(FAKE_STORE_ACCEPTS);
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(store_holds("spisor/github/token", value));
    spisor_memory_release(value);
}

static void a_malformed_file_is_never_overwritten(void) {
    scratch place;
    fresh_scratch(&place, "malformed");
    write_raw_file(&place, "{ not json");
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses(SECRETS_SET, request_for(&place, "github", "token", "v1")));
    CHECK(refuses(SECRETS_CLEAR, request_for(&place, "github", NULL, NULL)));
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(place.file, &length);
    check_text((const char *)bytes, "{ not json", "malformed file");
    spisor_memory_release(bytes);
}

static void unusable_arguments_are_refused(void) {
    scratch place;
    fresh_scratch(&place, "refusals");
    CHECK(malformed(SECRETS_GET, request_for(&place, "github", NULL, NULL)));
    CHECK(malformed(SECRETS_GET, request_for(&place, "github", "", NULL)));
    CHECK(malformed(SECRETS_GET, request_for(&place, NULL, "token", NULL)));
    CHECK(malformed(SECRETS_GET, request_for(&place, "", "token", NULL)));
    CHECK(malformed(SECRETS_GET, request_for(&place, "git/hub", "token", NULL)));
    CHECK(malformed(SECRETS_SET, request_for(&place, "github", "token", NULL)));
    CHECK(malformed(SECRETS_REMOVE, request_for(&place, "github", "", NULL)));
    CHECK(malformed(SECRETS_CLEAR, request_for(&place, NULL, NULL, NULL)));
    request relative = request_for(&place, "a", "b", NULL);
    relative.relative_file = "rel/secrets.json";
    CHECK(malformed(SECRETS_GET, relative));
    CHECK(malformed(SECRETS_RECONCILE, request_for(NULL, NULL, NULL, NULL)));
    CHECK(fake_store_calls() == 0);
}

static void an_argument_or_answer_short_of_its_shape_is_malformed(void) {
    const runtime_secrets_definition *secrets = secrets_definition();
    scratch place;
    runtime_secret_places places = SPISOR_INIT(runtime_secret_places);
    runtime_secrets_entry_argument entry = SPISOR_INIT(runtime_secrets_entry_argument);
    runtime_secrets_get_answer secret = SPISOR_INIT(runtime_secrets_get_answer);
    runtime_secrets_set_argument set = SPISOR_INIT(runtime_secrets_set_argument);
    runtime_secrets_set_answer stored = SPISOR_INIT(runtime_secrets_set_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    fresh_scratch(&place, "shapes");
    places.secrets_file = place.file;
    places.force_file_store = 1;
    entry.places = set.places = &places;
    entry.plugin = set.plugin = "github";
    entry.name = set.name = "token";
    set.value = "v1";
    entry.size = (uint32_t)offsetof(runtime_secrets_entry_argument, name);
    check_refused(secrets->get(secrets->context, &entry, &secret, &refusal), SPISOR_MALFORMED, &refusal,
                  "first shape", "a short entry");
    secrets->release_refusal(secrets->context, &refusal);
    entry.size = sizeof entry;
    secret.size = (uint32_t)offsetof(runtime_secrets_get_answer, storage);
    check_refused(secrets->get(secrets->context, &entry, &secret, &refusal), SPISOR_MALFORMED, &refusal,
                  "first shape", "a short secret answer");
    secrets->release_refusal(secrets->context, &refusal);
    stored.size = (uint32_t)offsetof(runtime_secrets_set_answer, location);
    check_refused(secrets->set(secrets->context, &set, &stored, &refusal), SPISOR_MALFORMED, &refusal, "first shape",
                  "a short set answer");
    secrets->release_refusal(secrets->context, &refusal);
    places.size = (uint32_t)offsetof(runtime_secret_places, force_file_store);
    entry.size = sizeof entry;
    secret.size = sizeof secret;
    check_refused(secrets->get(secrets->context, &entry, &secret, &refusal), SPISOR_MALFORMED, &refusal, "places",
                  "places short of their first shape");
    secrets->release_refusal(secrets->context, &refusal);
    CHECK(spisor_probe_file(place.file) != SPISOR_FILE_PRESENT);
}

static void the_definition_is_one_static_struct(void) {
    const runtime_secrets_definition *secrets = secrets_definition();
    CHECK(secrets != NULL && (const void *)secrets == runtime_secrets_definition_of());
    CHECK(secrets != NULL && secrets->size == sizeof(runtime_secrets_definition));
}

#if !defined(_WIN32)
static void the_file_is_owner_only(void) {
    scratch place;
    fresh_scratch(&place, "owner-only");
    mode_t previous = umask(0);
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "v1", "file");
    umask(previous);
    struct stat status;
    CHECK(stat(place.file, &status) == 0 && (status.st_mode & 07777) == 0600);
}
#endif

static void a_stored_set_beside_an_unreadable_file_is_refused_and_leaves_the_file(void) {
    scratch place;
    fresh_scratch(&place, "stored-unreadable");
    write_raw_file(&place, "{ not json");
    CHECK(refuses_naming(SECRETS_SET, request_for(&place, "github", "token", "v1"), "message",
                         "not a readable JSON object"));
    expect_file_bytes(place.file, "{ not json", "the unreadable file is left byte-identical");
    CHECK(fake_store_set_calls() == 0);
    CHECK(fake_store_entry_count() == 0);
}

#if defined(_WIN32)
static int make_file_unwritable(const scratch *place, int unwritable) {
    wchar_t wide[1100];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, place->file, -1, wide, 1100) == 0) {
        return 0;
    }
    return SetFileAttributesW(wide, unwritable ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL) != 0;
}
#else
/** @implNote A process running as root writes into a read-only directory anyway, so there it answers 0. */
static int make_file_unwritable(const scratch *place, int unwritable) {
    char directory[1100];
    snprintf(directory, sizeof directory, "%s/state", place->directory);
    if (chmod(directory, unwritable ? 0555 : 0755) != 0) {
        return 0;
    }
    return !unwritable || access(directory, W_OK) != 0;
}
#endif

static int unwritable_or_skip(const scratch *place) {
    if (make_file_unwritable(place, 1)) {
        return 1;
    }
    make_file_unwritable(place, 0);
    printf("SKIP unwritable secrets file: this process can write a read-only directory\n");
    return 0;
}

static void a_stored_set_whose_older_file_copy_cannot_leave_is_refused(void) {
    scratch place;
    fresh_scratch(&place, "stale-copy");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "old", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    if (!unwritable_or_skip(&place)) {
        return;
    }
    CHECK(refuses_naming(SECRETS_SET, request_for(&place, "github", "token", "new"), "message",
                         "its older copy in the secrets file could not be removed"));
    CHECK(make_file_unwritable(&place, 0));
    CHECK(store_holds("spisor/github/token", "new"));
    CHECK(file_holds(&place, "spisor/github/token", "old"));
}

static void a_move_whose_file_rewrite_fails_leaves_the_entry_in_both_places_and_is_refused(void) {
    scratch place;
    fresh_scratch(&place, "both-places");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "v1", "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    if (!unwritable_or_skip(&place)) {
        return;
    }
    CHECK(refuses_naming(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL), "message",
                         "could not be written"));
    CHECK(make_file_unwritable(&place, 0));
    CHECK(store_holds("spisor/github/token", "v1"));
    CHECK(file_holds(&place, "spisor/github/token", "v1"));
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(file_entry_count(&place) == 0);
    expect_get(&place, "github", "token", "v1");
}

static void a_declined_delete_keeps_the_newer_file_copy(void) {
    scratch place;
    fresh_scratch(&place, "declined-delete");
    set_secret(&place, "github", "token", "A", "store");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "B", "file");
    CHECK(refuses(SECRETS_REMOVE, request_for(&place, "github", "token", NULL)));
    CHECK(file_holds(&place, "spisor/github/token", "B"));
    fake_store_behave(FAKE_STORE_ACCEPTS);
    expect_get(&place, "github", "token", "B");
}

static void an_over_cap_value_is_refused_before_either_place_is_touched(void) {
    scratch place;
    fresh_scratch(&place, "over-cap");
    char *over = repeated_value(VALUE_CAP_BYTES + 1);
    CHECK(over != NULL);
    CHECK(refuses_naming(SECRETS_SET, request_for(&place, "github", "token", over), "message", "2560000"));
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses_naming(SECRETS_SET, request_for(&place, "github", "token", over), "message", "2560000"));
    CHECK(fake_store_calls() == 0);
    CHECK(fake_store_entry_count() == 0);
    CHECK(spisor_probe_file(place.file) != SPISOR_FILE_PRESENT);
    spisor_memory_release(over);
    char *at_cap = repeated_value(VALUE_CAP_BYTES);
    CHECK(at_cap != NULL);
    set_secret(&place, "github", "token", at_cap, "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    set_secret(&place, "github", "token", at_cap, "store");
    CHECK(store_holds("spisor/github/token", at_cap));
    spisor_memory_release(at_cap);
}

static void a_hand_planted_over_cap_file_entry_never_stops_the_move(void) {
    scratch place;
    fresh_scratch(&place, "planted-over-cap");
    char *over = repeated_value(VALUE_CAP_BYTES + 1);
    cJSON *entries = cJSON_CreateObject();
    cJSON_AddStringToObject(entries, "spisor/github/huge", over);
    cJSON_AddStringToObject(entries, "spisor/github/small", "s");
    char *text = cJSON_PrintUnformatted(entries);
    cJSON_Delete(entries);
    write_raw_file(&place, text);
    spisor_memory_release(text);
    CHECK(moved_count(call(SECRETS_RECONCILE, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(store_holds("spisor/github/small", "s"));
    CHECK(fake_store_value("spisor/github/huge") == NULL);
    CHECK(file_holds(&place, "spisor/github/huge", NULL));
    CHECK(file_entry_count(&place) == 1);
    set_secret(&place, "gitlab", "other", "o", "store");
    CHECK(file_entry_count(&place) == 1);
    spisor_memory_release(over);
}

static void a_linked_secrets_file_is_never_written_through(void) {
    scratch place;
    fresh_scratch(&place, "linked");
    char dotfiles[1100];
    char target[1200];
    char state[1100];
    snprintf(dotfiles, sizeof dotfiles, "%s/dotfiles", place.directory);
    snprintf(target, sizeof target, "%s/secrets.json", dotfiles);
    snprintf(state, sizeof state, "%s/state", place.directory);
    CHECK(spisor_make_directories(dotfiles) == 1);
    CHECK(spisor_make_directories(state) == 1);
    CHECK(spisor_replace_file(target, (const uint8_t *)"{}", 2) == SPISOR_OK);
    if (!create_link_or_skip(place.file, target)) {
        return;
    }
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses_naming(SECRETS_SET, request_for(&place, "github", "token", "v1"), "fix", "SPISOR_SECRETS"));
    CHECK(is_link(place.file));
    expect_file_bytes(target, "{}", "the link's target is left unchanged");
}

int main(void) {
    a_declined_set_lands_in_the_file();
    an_unavailable_store_sends_the_set_to_the_file();
    a_failing_store_fails_the_set_and_writes_nothing();
    a_stored_set_moves_every_file_entry_and_empties_the_file();
    a_stored_set_is_not_overwritten_by_its_older_file_copy();
    a_move_failing_partway_loses_nothing_and_duplicates_nothing();
    get_prefers_the_file_and_never_writes_to_the_store();
    a_value_set_while_the_store_declines_is_not_shadowed_by_its_stored_older_value();
    get_fails_when_neither_place_holds_it_and_the_store_refused();
    delete_removes_from_both_places();
    clear_removes_one_plugin_from_both_places_and_keeps_another();
    a_forced_file_store_never_touches_the_store();
    reconcile_moves_file_entries_once_the_store_appears();
    reconcile_with_no_file_moves_nothing();
    a_long_utf8_value_round_trips_through_the_file_and_the_move();
    a_malformed_file_is_never_overwritten();
    unusable_arguments_are_refused();
    an_argument_or_answer_short_of_its_shape_is_malformed();
    the_definition_is_one_static_struct();
    a_stored_set_beside_an_unreadable_file_is_refused_and_leaves_the_file();
    a_stored_set_whose_older_file_copy_cannot_leave_is_refused();
    a_move_whose_file_rewrite_fails_leaves_the_entry_in_both_places_and_is_refused();
    a_declined_delete_keeps_the_newer_file_copy();
    an_over_cap_value_is_refused_before_either_place_is_touched();
    a_hand_planted_over_cap_file_entry_never_stops_the_move();
    a_linked_secrets_file_is_never_written_through();
#if !defined(_WIN32)
    the_file_is_owner_only();
#endif
    fake_store_reset();
    return summary_with_links("secrets_test");
}
