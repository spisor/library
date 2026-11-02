#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

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
#include "scratch_directory.h"
#include "fake_secret_backend.h"
#include "links.h"
#include "secrets.h"
#include "spisor/file.h"
#include "spisor/json_file.h"
#include "spisor/json_kind.h"

typedef int32_t (*secrets_export)(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len);

typedef struct scratch {
    char directory[1024];
    char file[1100];
    int force_file_store;
} scratch;

#define VALUE_CAP_BYTES 2560000u

typedef struct answer {
    int32_t status;
    cJSON *tree;
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

static cJSON *request_for(const scratch *place, const char *plugin, const char *name, const char *value) {
    cJSON *request = cJSON_CreateObject();
    cJSON *places = cJSON_AddObjectToObject(request, "places");
    cJSON_AddStringToObject(places, "secretsFile", place->file);
    cJSON_AddBoolToObject(places, "forceFileStore", place->force_file_store);
    if (plugin != NULL) {
        cJSON_AddStringToObject(request, "plugin", plugin);
    }
    if (name != NULL) {
        cJSON_AddStringToObject(request, "name", name);
    }
    if (value != NULL) {
        cJSON_AddStringToObject(request, "value", value);
    }
    return request;
}

static answer call_text(secrets_export function, const char *text) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    answer result;
    result.status = function((const uint8_t *)text, strlen(text), &out, &out_len);
    result.tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
    return result;
}

static answer call(secrets_export function, cJSON *request) {
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    answer result = call_text(function, text);
    free(text);
    return result;
}

static int succeeds(secrets_export function, cJSON *request) {
    answer result = call(function, request);
    int succeeded = result.status == SPISOR_STATUS_SUCCESS;
    cJSON_Delete(result.tree);
    return succeeded;
}

static int refuses(secrets_export function, cJSON *request) {
    answer result = call(function, request);
    int refused = result.status == SPISOR_STATUS_FAILURE &&
                  cJSON_IsString(cJSON_GetObjectItemCaseSensitive(result.tree, "message"));
    cJSON_Delete(result.tree);
    return refused;
}

static int refuses_naming(secrets_export function, cJSON *request, const char *member, const char *text) {
    answer result = call(function, request);
    const char *said = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, member));
    int refused = result.status == SPISOR_STATUS_FAILURE && said != NULL && strstr(said, text) != NULL;
    if (!refused) {
        fprintf(stderr, "expected a refusal whose %s names \"%s\", got status %d and \"%s\"\n", member, text,
                (int)result.status, said == NULL ? "(none)" : said);
    }
    cJSON_Delete(result.tree);
    return refused;
}

static void expect_file_bytes(const char *path, const char *expected, const char *label) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    check_text((const char *)bytes, expected, label);
    free(bytes);
}

static char *repeated_value(size_t length) {
    char *value = malloc(length + 1);
    if (value != NULL) {
        memset(value, 'x', length);
        value[length] = '\0';
    }
    return value;
}

static void set_secret(const scratch *place, const char *plugin, const char *name, const char *value,
                       const char *expected_place) {
    answer result = call(runtime_secrets_set, request_for(place, plugin, name, value));
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    check_text(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "place")), expected_place, name);
    cJSON_Delete(result.tree);
}

/** The value get answers, as a copy freed with free, or NULL when it answers none. */
static char *get_secret(const scratch *place, const char *plugin, const char *name) {
    answer result = call(runtime_secrets_get, request_for(place, plugin, name, NULL));
    CHECK(result.status == SPISOR_STATUS_SUCCESS);
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result.tree, "value"));
    char *copy = value == NULL ? NULL : malloc(strlen(value) + 1);
    if (copy != NULL) {
        memcpy(copy, value, strlen(value) + 1);
    }
    cJSON_Delete(result.tree);
    return copy;
}

static void expect_get(const scratch *place, const char *plugin, const char *name, const char *expected) {
    char *value = get_secret(place, plugin, name);
    if (expected == NULL) {
        CHECK(value == NULL);
    } else {
        check_text(value, expected, name);
    }
    free(value);
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
    CHECK(spisor_replace_file(place->file, (const uint8_t *)content, strlen(content)) == 1);
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
    CHECK(refuses(runtime_secrets_set, request_for(&place, "github", "token", "v1")));
    CHECK(!spisor_file_exists(place.file));
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
    CHECK(refuses(runtime_secrets_get, request_for(&place, "github", "token", NULL)));
    fake_store_behave(FAKE_STORE_FAILS);
    CHECK(refuses(runtime_secrets_get, request_for(&place, "github", "token", NULL)));
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
    CHECK(succeeds(runtime_secrets_delete, request_for(&place, "github", "token", NULL)));
    CHECK(fake_store_value("spisor/github/token") == NULL);
    CHECK(!file_holds(&place, "spisor/github/token", NULL));
    expect_get(&place, "github", "token", NULL);
    CHECK(succeeds(runtime_secrets_delete, request_for(&place, "github", "token", NULL)));
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
    CHECK(succeeds(runtime_secrets_clear, request_for(&place, "github", NULL, NULL)));
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
    CHECK(succeeds(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL)));
    CHECK(succeeds(runtime_secrets_delete, request_for(&place, "github", "token", NULL)));
    set_secret(&place, "github", "other", "v2", "file");
    CHECK(succeeds(runtime_secrets_clear, request_for(&place, "github", NULL, NULL)));
    CHECK(fake_store_calls() == 0);
    CHECK(file_entry_count(&place) == 0);
}

static int moved_count(answer result) {
    const cJSON *moved = cJSON_GetObjectItemCaseSensitive(result.tree, "moved");
    int count = result.status == SPISOR_STATUS_SUCCESS && cJSON_IsNumber(moved) ? moved->valueint : -1;
    cJSON_Delete(result.tree);
    return count;
}

static void reconcile_moves_file_entries_once_the_store_appears(void) {
    scratch place;
    fresh_scratch(&place, "reconcile");
    fake_store_behave(FAKE_STORE_UNAVAILABLE);
    set_secret(&place, "github", "one", "1", "file");
    set_secret(&place, "gitlab", "two", "2", "file");
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 0);
    CHECK(file_entry_count(&place) == 2);
    fake_store_behave(FAKE_STORE_ACCEPTS);
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 2);
    CHECK(store_holds("spisor/github/one", "1"));
    CHECK(store_holds("spisor/gitlab/two", "2"));
    CHECK(file_entry_count(&place) == 0);
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 0);
}

static void reconcile_with_no_file_moves_nothing(void) {
    scratch place;
    fresh_scratch(&place, "reconcile-empty");
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 0);
    CHECK(!spisor_file_exists(place.file));
}

static char *long_utf8_value(size_t repeats) {
    const char *unit = "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x94\x91 ";
    size_t unit_length = strlen(unit);
    char *value = malloc(unit_length * repeats + 1);
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
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(store_holds("spisor/github/token", value));
    free(value);
}

static void a_malformed_file_is_never_overwritten(void) {
    scratch place;
    fresh_scratch(&place, "malformed");
    write_raw_file(&place, "{ not json");
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses(runtime_secrets_set, request_for(&place, "github", "token", "v1")));
    CHECK(refuses(runtime_secrets_clear, request_for(&place, "github", NULL, NULL)));
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(place.file, &length);
    check_text((const char *)bytes, "{ not json", "malformed file");
    free(bytes);
}

static void unusable_arguments_are_refused(void) {
    scratch place;
    fresh_scratch(&place, "refusals");
    CHECK(refuses(runtime_secrets_get, request_for(&place, "github", NULL, NULL)));
    CHECK(refuses(runtime_secrets_get, request_for(&place, "github", "", NULL)));
    CHECK(refuses(runtime_secrets_get, request_for(&place, NULL, "token", NULL)));
    CHECK(refuses(runtime_secrets_get, request_for(&place, "", "token", NULL)));
    CHECK(refuses(runtime_secrets_get, request_for(&place, "git/hub", "token", NULL)));
    CHECK(refuses(runtime_secrets_set, request_for(&place, "github", "token", NULL)));
    CHECK(refuses(runtime_secrets_clear, request_for(&place, NULL, NULL, NULL)));
    answer relative = call_text(runtime_secrets_get, "{\"places\":{\"secretsFile\":\"rel/secrets.json\","
                                                     "\"forceFileStore\":false},\"plugin\":\"a\",\"name\":\"b\"}");
    CHECK(relative.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(relative.tree);
    answer no_places = call_text(runtime_secrets_reconcile, "{}");
    CHECK(no_places.status == SPISOR_STATUS_FAILURE);
    cJSON_Delete(no_places.tree);
    CHECK(fake_store_calls() == 0);
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
    CHECK(refuses_naming(runtime_secrets_set, request_for(&place, "github", "token", "v1"), "message",
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
    CHECK(refuses_naming(runtime_secrets_set, request_for(&place, "github", "token", "new"), "message",
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
    CHECK(refuses_naming(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL), "message",
                         "could not be written"));
    CHECK(make_file_unwritable(&place, 0));
    CHECK(store_holds("spisor/github/token", "v1"));
    CHECK(file_holds(&place, "spisor/github/token", "v1"));
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(file_entry_count(&place) == 0);
    expect_get(&place, "github", "token", "v1");
}

static void a_declined_delete_keeps_the_newer_file_copy(void) {
    scratch place;
    fresh_scratch(&place, "declined-delete");
    set_secret(&place, "github", "token", "A", "store");
    fake_store_behave(FAKE_STORE_DECLINES);
    set_secret(&place, "github", "token", "B", "file");
    CHECK(refuses(runtime_secrets_delete, request_for(&place, "github", "token", NULL)));
    CHECK(file_holds(&place, "spisor/github/token", "B"));
    fake_store_behave(FAKE_STORE_ACCEPTS);
    expect_get(&place, "github", "token", "B");
}

static void an_over_cap_value_is_refused_before_either_place_is_touched(void) {
    scratch place;
    fresh_scratch(&place, "over-cap");
    char *over = repeated_value(VALUE_CAP_BYTES + 1);
    CHECK(over != NULL);
    CHECK(refuses_naming(runtime_secrets_set, request_for(&place, "github", "token", over), "message", "2560000"));
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses_naming(runtime_secrets_set, request_for(&place, "github", "token", over), "message", "2560000"));
    CHECK(fake_store_calls() == 0);
    CHECK(fake_store_entry_count() == 0);
    CHECK(!spisor_file_exists(place.file));
    free(over);
    char *at_cap = repeated_value(VALUE_CAP_BYTES);
    CHECK(at_cap != NULL);
    set_secret(&place, "github", "token", at_cap, "file");
    fake_store_behave(FAKE_STORE_ACCEPTS);
    set_secret(&place, "github", "token", at_cap, "store");
    CHECK(store_holds("spisor/github/token", at_cap));
    free(at_cap);
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
    free(text);
    CHECK(moved_count(call(runtime_secrets_reconcile, request_for(&place, NULL, NULL, NULL))) == 1);
    CHECK(store_holds("spisor/github/small", "s"));
    CHECK(fake_store_value("spisor/github/huge") == NULL);
    CHECK(file_holds(&place, "spisor/github/huge", NULL));
    CHECK(file_entry_count(&place) == 1);
    set_secret(&place, "gitlab", "other", "o", "store");
    CHECK(file_entry_count(&place) == 1);
    free(over);
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
    CHECK(spisor_replace_file(target, (const uint8_t *)"{}", 2) == 1);
    if (!create_link_or_skip(place.file, target)) {
        return;
    }
    fake_store_behave(FAKE_STORE_DECLINES);
    CHECK(refuses_naming(runtime_secrets_set, request_for(&place, "github", "token", "v1"), "fix", "SPISOR_SECRETS"));
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
