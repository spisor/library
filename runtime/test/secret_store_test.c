#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#include <process.h>
#define BOUNDARY_LIMIT_BYTES CRED_MAX_CREDENTIAL_BLOB_SIZE
#define CURRENT_PROCESS_ID _getpid()
#else
#include <unistd.h>
#define BOUNDARY_LIMIT_BYTES 2560
#define CURRENT_PROCESS_ID getpid()
#endif

#include "check.h"
#include "secret_backend.h"
#ifdef _WIN32
#include "secret_windows_fault.h"
#endif

#define SKIPPED_EXIT_CODE 77
#define FIRST_FILLER 'x'
#define ENTRY_CAPACITY 320

static char plugin_id[96];
static char prefix[128];

static void choose_unique_prefix(void) {
    snprintf(plugin_id, sizeof plugin_id, "storetest-%d-%lld", (int)CURRENT_PROCESS_ID, (long long)time(NULL));
    snprintf(prefix, sizeof prefix, "spisor/%s/", plugin_id);
}

static char *entry_for(const char *name) {
    static char entries[8][ENTRY_CAPACITY];
    static int next = 0;
    char *entry = entries[next++ % 8];
    snprintf(entry, ENTRY_CAPACITY, "%s%s", prefix, name);
    return entry;
}

static int stores(const char *name, const char *value) {
    runtime_secret_answer answer = runtime_secret_store_set(entry_for(name), value);
    CHECK(answer == RUNTIME_SECRET_DONE);
    return answer == RUNTIME_SECRET_DONE;
}

static int reads_back(const char *name, const char *expected) {
    char *value = NULL;
    runtime_secret_answer answer = runtime_secret_store_get(entry_for(name), &value);
    int same = answer == RUNTIME_SECRET_DONE && value != NULL && strcmp(value, expected) == 0;
    CHECK(same);
    free(value);
    return same;
}

static int is_absent(const char *name) {
    char *value = NULL;
    runtime_secret_answer answer = runtime_secret_store_get(entry_for(name), &value);
    free(value);
    return answer == RUNTIME_SECRET_ABSENT;
}

static int listed_count(const char *under) {
    runtime_secret_names names = {NULL, 0};
    char search[ENTRY_CAPACITY];
    snprintf(search, sizeof search, "%s%s", prefix, under);
    CHECK(runtime_secret_store_list(search, &names) == RUNTIME_SECRET_DONE);
    int count = (int)names.count;
    runtime_free_secret_names(&names);
    return count;
}

static int listing_holds(const char *under, const char *name) {
    runtime_secret_names names = {NULL, 0};
    char search[ENTRY_CAPACITY];
    int found = 0;
    snprintf(search, sizeof search, "%s%s", prefix, under);
    CHECK(runtime_secret_store_list(search, &names) == RUNTIME_SECRET_DONE);
    for (size_t index = 0; index < names.count; index++) {
        found |= strcmp(names.names[index], entry_for(name)) == 0;
    }
    runtime_free_secret_names(&names);
    return found;
}

#ifdef _WIN32
static int chunk_root_entries(int remove) {
    wchar_t filter[256];
    DWORD count = 0;
    PCREDENTIALW *credentials = NULL;
    swprintf(filter, 256, L"spisor-chunks/%hs/*", plugin_id);
    if (!CredEnumerateW(filter, 0, &count, &credentials)) {
        return 0;
    }
    for (DWORD index = 0; remove && index < count; index++) {
        CredDeleteW(credentials[index]->TargetName, CRED_TYPE_GENERIC, 0);
    }
    CredFree(credentials);
    return (int)count;
}
#else
static int chunk_root_entries(int remove) {
    (void)remove;
    return 0;
}
#endif

static void remove_everything_written(void) {
    runtime_secret_names names = {NULL, 0};
    if (runtime_secret_store_list(prefix, &names) == RUNTIME_SECRET_DONE) {
        for (size_t index = 0; index < names.count; index++) {
            runtime_secret_store_delete(names.names[index]);
        }
    }
    runtime_free_secret_names(&names);
    chunk_root_entries(1);
}

#ifdef _WIN32
static char *filled_value(size_t length) {
    char *value = malloc(length + 1);
    for (size_t index = 0; index < length; index++) {
        value[index] = (char)(FIRST_FILLER + (index % 23));
    }
    value[length] = '\0';
    return value;
}

static char *value_with_character_across_boundary(size_t length) {
    char *value = filled_value(length);
    value[BOUNDARY_LIMIT_BYTES - 1] = '\xC3';
    value[BOUNDARY_LIMIT_BYTES] = '\xA9';
    return value;
}

static int chunk_in_generation(const char *name, int generation, int number) {
    wchar_t target[512];
    PCREDENTIALW credential = NULL;
    swprintf(target, 512, L"spisor-chunks/%hs/%hs#%d#%d", plugin_id, name, generation, number);
    int exists = CredReadW(target, CRED_TYPE_GENERIC, 0, &credential) != 0;
    if (exists) {
        CredFree(credential);
    }
    return exists;
}

static int chunk_exists(const char *name, int number) {
    return chunk_in_generation(name, 1, number) || chunk_in_generation(name, 2, number);
}

static void plant_orphan(const char *name, int generation, int number) {
    wchar_t target[512];
    CREDENTIALW credential;
    memset(&credential, 0, sizeof credential);
    swprintf(target, 512, L"spisor-chunks/%hs/%hs#%d#%d", plugin_id, name, generation, number);
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target;
    credential.CredentialBlobSize = 5;
    credential.CredentialBlob = (LPBYTE)"stale";
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    CHECK(CredWriteW(&credential, 0));
}

static int fails_midway(const char *name, const char *value, long writes_before_failure) {
    runtime_secret_windows_allow_chunk_writes(writes_before_failure);
    runtime_secret_answer answer = runtime_secret_store_set(entry_for(name), value);
    runtime_secret_windows_allow_chunk_writes(-1);
    return answer == RUNTIME_SECRET_FAILED;
}

static void an_interrupted_chunked_rewrite_keeps_the_old_value(void) {
    char *old_value = filled_value(6000);
    char *new_value = value_with_character_across_boundary(7000);
    if (stores("fault", old_value)) {
        int before = chunk_root_entries(0);
        CHECK(fails_midway("fault", new_value, 1));
        reads_back("fault", old_value);
        CHECK(chunk_root_entries(0) == before);
        CHECK(stores("fault", new_value));
        reads_back("fault", new_value);
    }
    free(old_value);
    free(new_value);
}

static void an_unreadable_base_leaves_the_old_chunks_untouched(void) {
    char *old_value = filled_value(6000);
    char *new_value = filled_value(7000);
    if (stores("unreadable", old_value)) {
        int before = chunk_root_entries(0);
        runtime_secret_windows_allow_base_reads(0);
        CHECK(runtime_secret_store_set(entry_for("unreadable"), new_value) == RUNTIME_SECRET_FAILED);
        runtime_secret_windows_allow_base_reads(-1);
        reads_back("unreadable", old_value);
        CHECK(chunk_root_entries(0) == before);
    }
    free(old_value);
    free(new_value);
}

static void an_interrupted_chunked_set_over_a_whole_value_keeps_it(void) {
    char *large = filled_value(6000);
    if (stores("whole", "small") && fails_midway("whole", large, 2)) {
        reads_back("whole", "small");
        CHECK(!chunk_exists("whole", 1));
    }
    free(large);
}

static int chunks_of_other_tests = 0;

static void plant_orphans(const char *name) {
    plant_orphan(name, 1, 1);
    plant_orphan(name, 1, 2);
    plant_orphan(name, 1, 3);
    plant_orphan(name, 1, 4);
    plant_orphan(name, 2, 1);
    plant_orphan(name, 2, 2);
}

static void a_rewrite_after_a_failed_sweep_reads_correctly(void) {
    char *large = filled_value(6000);
    chunks_of_other_tests = chunk_root_entries(0);
    plant_orphans("swept");
    if (stores("swept", large)) {
        reads_back("swept", large);
        CHECK(chunk_root_entries(0) == chunks_of_other_tests + 3);
    }
    plant_orphans("swept");
    CHECK(runtime_secret_store_delete(entry_for("swept")) == RUNTIME_SECRET_DONE);
    CHECK(chunk_root_entries(0) == chunks_of_other_tests);
    free(large);
}

static void a_value_over_the_cap_is_refused_and_the_old_one_kept(void) {
    size_t too_long = 1000 * (size_t)BOUNDARY_LIMIT_BYTES + 1;
    char *huge = filled_value(too_long);
    if (stores("capped", "kept")) {
        CHECK(runtime_secret_store_set(entry_for("capped"), huge) == RUNTIME_SECRET_FAILED);
        reads_back("capped", "kept");
    }
    free(huge);
}

static void a_value_over_one_entry_round_trips_whole(void) {
    char *large = value_with_character_across_boundary(6000);
    if (stores("large", large)) {
        reads_back("large", large);
        CHECK(chunk_exists("large", 1) && chunk_exists("large", 3) && !chunk_exists("large", 4));
    }
    free(large);
}

static void a_shorter_rewrite_deletes_the_surplus_chunks(void) {
    char *medium = filled_value(3000);
    CHECK(chunk_exists("large", 3));
    if (stores("large", medium)) {
        reads_back("large", medium);
        CHECK(chunk_exists("large", 2) && !chunk_exists("large", 3));
    }
    if (stores("large", "short now")) {
        reads_back("large", "short now");
        CHECK(!chunk_exists("large", 1) && !chunk_exists("large", 2));
    }
    free(medium);
}

static void a_value_of_exactly_one_entry_is_not_chunked(void) {
    char *exact = filled_value(BOUNDARY_LIMIT_BYTES);
    char *one_over = filled_value(BOUNDARY_LIMIT_BYTES + 1);
    if (stores("exact", exact)) {
        reads_back("exact", exact);
        CHECK(!chunk_exists("exact", 1));
    }
    if (stores("exact", one_over)) {
        reads_back("exact", one_over);
        CHECK(chunk_exists("exact", 1) && chunk_exists("exact", 2) && !chunk_exists("exact", 3));
    }
    free(exact);
    free(one_over);
}

static void deleting_a_logical_entry_deletes_its_chunks(void) {
    char *large = filled_value(6000);
    if (stores("doomed", large)) {
        CHECK(chunk_exists("doomed", 3));
        CHECK(runtime_secret_store_delete(entry_for("doomed")) == RUNTIME_SECRET_DONE);
        CHECK(!chunk_exists("doomed", 1) && !chunk_exists("doomed", 3));
        CHECK(is_absent("doomed"));
    }
    free(large);
}

static void chunks_never_appear_in_a_listing(void) {
    char *large = filled_value(6000);
    if (stores("listed#2", large)) {
        CHECK(listing_holds("", "listed#2"));
        CHECK(listed_count("listed") == 1);
    }
    free(large);
}

static void windows_only_checks(void) {
    a_value_over_one_entry_round_trips_whole();
    a_shorter_rewrite_deletes_the_surplus_chunks();
    a_value_of_exactly_one_entry_is_not_chunked();
    deleting_a_logical_entry_deletes_its_chunks();
    chunks_never_appear_in_a_listing();
    an_interrupted_chunked_rewrite_keeps_the_old_value();
    an_unreadable_base_leaves_the_old_chunks_untouched();
    an_interrupted_chunked_set_over_a_whole_value_keeps_it();
    a_rewrite_after_a_failed_sweep_reads_correctly();
    a_value_over_the_cap_is_refused_and_the_old_one_kept();
}
#else
static void windows_only_checks(void) {}
#endif

static void set_get_overwrite_and_delete(void) {
    if (stores("token", "first")) {
        reads_back("token", "first");
    }
    if (stores("token", "second \xC3\xA9 value")) {
        reads_back("token", "second \xC3\xA9 value");
    }
    CHECK(runtime_secret_store_delete(entry_for("token")) == RUNTIME_SECRET_DONE);
    CHECK(is_absent("token"));
    CHECK(runtime_secret_store_delete(entry_for("token")) == RUNTIME_SECRET_ABSENT);
}

static void an_empty_value_round_trips(void) {
    if (stores("blank", "")) {
        reads_back("blank", "");
    }
}

static void list_answers_only_names_under_the_prefix(void) {
    if (stores("a", "1") && stores("b", "2") && stores("nested/c", "3")) {
        CHECK(listed_count("nested/") == 1);
        CHECK(listing_holds("nested/", "nested/c"));
        CHECK(listing_holds("", "a") && listing_holds("", "b") && listing_holds("", "nested/c"));
    }
}

static void names_with_slash_and_hash_stay_distinct(void) {
    if (stores("x/y#1", "one") && stores("x/y", "two")) {
        reads_back("x/y#1", "one");
        reads_back("x/y", "two");
    }
}

#ifdef __APPLE__
static int the_store_is_unusable_here(void) {
    runtime_secret_answer answer = runtime_secret_store_set(entry_for("probe"), "probe");
    if (answer != RUNTIME_SECRET_UNAVAILABLE) {
        return 0;
    }
    fprintf(stderr, "secret_store_test: the macOS keychain answered unavailable in this session\n");
    return 1;
}

static int skip_is_forbidden(void) {
    return getenv("CI") != NULL && getenv("CI")[0] != '\0';
}
#else
static int the_store_is_unusable_here(void) {
    return 0;
}

static int skip_is_forbidden(void) {
    return 1;
}
#endif

int main(void) {
    choose_unique_prefix();
    if (the_store_is_unusable_here()) {
        remove_everything_written();
        if (skip_is_forbidden()) {
            fprintf(stderr, "secret_store_test: FAILING because CI is set; a runner must provide a usable keychain\n");
            return 1;
        }
        fprintf(stderr, "secret_store_test: SKIPPED, the real store was not exercised\n");
        return SKIPPED_EXIT_CODE;
    }
    set_get_overwrite_and_delete();
    an_empty_value_round_trips();
    names_with_slash_and_hash_stay_distinct();
    list_answers_only_names_under_the_prefix();
    windows_only_checks();
    remove_everything_written();
    CHECK(listed_count("") == 0);
    CHECK(chunk_root_entries(0) == 0);
    return check_summary("secret_store_test");
}
