#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "cache_damaged.h"
#include "catalog_call.h"
#include "catalog_path.h"
#include "catalog_url.h"
#include "check.h"
#include "directory_tree.h"
#include "hosts_lock.h"
#include "paths.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#define current_process_id() _getpid()
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#define PLUGIN_JSON "{\"id\":\"a\",\"api\":1,\"version\":\"1.0.0\",\"entry\":\"index.js\"}"
#define INDEX_JS "export default {};"
#define LIBRARY_SO "elf"
#define HOUR_MILLISECONDS 3600000.0

static char scratch[1024];
static char cache[1400];
static char state[1400];

static char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static char *in_cache(const char *rest) {
    return joined(cache, rest);
}

static int exists_in_cache(const char *rest) {
    char *path = in_cache(rest);
    int exists = spisor_file_exists(path);
    free(path);
    return exists;
}

static void write_file(const char *path, const char *content) {
    char *parent = joined(path, "..");
    CHECK(spisor_make_directories(parent) == 1);
    free(parent);
    CHECK(spisor_replace_file(path, (const uint8_t *)content, strlen(content)) == 1);
}

static void write_in(const char *base, const char *rest, const char *content) {
    char *path = joined(base, rest);
    write_file(path, content);
    free(path);
}

static char *read_text(const char *path) {
    size_t length = 0;
    return (char *)spisor_read_file(path, &length);
}

static void expect_file_text(const char *base, const char *rest, const char *expected, const char *label) {
    char *path = joined(base, rest);
    char *text = read_text(path);
    check_text(text, expected, label);
    free(text);
    free(path);
}

static void sha256_of_text(const char *text, char out[RUNTIME_SHA256_HEX_SIZE]) {
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)text, strlen(text));
    runtime_sha256_finish_hex(&hash, out);
}

/** A file url serving content, stored under scratch/sources/name, so the real fetch.get runs. */
static char *source_url(const char *name, const char *content) {
    char rest[256];
    snprintf(rest, sizeof rest, "sources/%s", name);
    char *path = joined(scratch, rest);
    write_file(path, content);
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, path);
    free(path);
    return url;
}

static void remove_source(const char *name) {
    char rest[256];
    snprintf(rest, sizeof rest, "sources/%s", name);
    char *path = joined(scratch, rest);
    CHECK(runtime_remove_file_entry(path) == RUNTIME_ENTRY_DONE);
    free(path);
}

/** A FileRef naming content, served from a source file called name holding served. */
static cJSON *reference_serving(const char *name, const char *content, const char *served, size_t size) {
    char sha256[RUNTIME_SHA256_HEX_SIZE];
    sha256_of_text(content, sha256);
    char *url = source_url(name, served);
    cJSON *reference = cJSON_CreateObject();
    cJSON_AddStringToObject(reference, "url", url);
    cJSON_AddStringToObject(reference, "sha256", sha256);
    cJSON_AddNumberToObject(reference, "size", (double)size);
    free(url);
    return reference;
}

static cJSON *reference_to(const char *name, const char *content) {
    return reference_serving(name, content, content, strlen(content));
}

static cJSON *version_with_files(cJSON *files) {
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddItemToObject(entry, "files", files);
    return entry;
}

static cJSON *item_of(const char *kind, const char *version, cJSON *entry) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "kind", kind);
    cJSON_AddStringToObject(item, "latest", version);
    cJSON *versions = cJSON_AddObjectToObject(item, "versions");
    cJSON_AddItemToObject(versions, version, entry);
    return item;
}

static cJSON *plugin_item(void) {
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "plugin.json", reference_to("plugin.json", PLUGIN_JSON));
    cJSON_AddItemToObject(files, "dist/index.js", reference_to("index.js", INDEX_JS));
    return item_of("plugin", "1.0.0", version_with_files(files));
}

static cJSON *linux_only_library(void) {
    cJSON *entry = cJSON_CreateObject();
    cJSON *platforms = cJSON_AddObjectToObject(entry, "platforms");
    cJSON *linux_x64 = cJSON_AddObjectToObject(platforms, "linux-x64");
    cJSON *files = cJSON_AddObjectToObject(linux_x64, "files");
    cJSON_AddItemToObject(files, "liba.so", reference_to("liba.so", LIBRARY_SO));
    return item_of("library", "2.0.0", entry);
}

static catalog_answer install(const char *id, cJSON *item, const char *version, const char *platform) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "id", id);
    cJSON_AddItemToObject(argument, "item", item);
    cJSON_AddStringToObject(argument, "version", version);
    if (platform != NULL) {
        cJSON_AddStringToObject(argument, "platform", platform);
    }
    return call_catalog(runtime_cache_install, argument);
}

static catalog_answer check_directory(const char *directory) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "directory", directory);
    return call_catalog(runtime_cache_check, argument);
}

static catalog_answer sweep(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "state", state);
    return call_catalog(runtime_cache_sweep, argument);
}

static catalog_answer installed(const char *platform) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    if (platform != NULL) {
        cJSON_AddStringToObject(argument, "platform", platform);
    }
    return call_catalog(runtime_cache_installed, argument);
}

static void expect_tree(const cJSON *actual, const char *expected_json, const char *label) {
    cJSON *expected = parsed(expected_json);
    if (actual == NULL || !cJSON_Compare(actual, expected, 1)) {
        char *printed = actual == NULL ? NULL : cJSON_PrintUnformatted(actual);
        fprintf(stderr, "%s: expected %s, got %s\n", label, expected_json, printed == NULL ? "(none)" : printed);
        cJSON_free(printed);
        check_failures++;
    }
    cJSON_Delete(expected);
}

static void expect_answer(catalog_answer answer, const char *expected_json, const char *label) {
    if (answer.status != SPISOR_STATUS_SUCCESS) {
        fprintf(stderr, "%s: refused: %s\n", label, answer.tree == NULL ? "(none)" : text_at(answer.tree, "message"));
        check_failures++;
    } else {
        expect_tree(answer.tree, expected_json, label);
    }
    cJSON_Delete(answer.tree);
}

static void expect_refused(catalog_answer answer, const char *fragment, const char *label) {
    expect_refusal(answer, fragment, label);
    cJSON_Delete(answer.tree);
}

static void expect_member(const cJSON *tree, const char *member, const char *expected_json, const char *label) {
    expect_tree(cJSON_GetObjectItemCaseSensitive(tree, member), expected_json, label);
}

static void expect_absent_in_cache(const char *rest, const char *label) {
    if (exists_in_cache(rest)) {
        fprintf(stderr, "%s: %s exists in the cache\n", label, rest);
        check_failures++;
    }
}

/** The names directly inside cache/rest, as a JSON array text, for comparing listings. */
static char *listing_of(const char *rest) {
    char *path = in_cache(rest);
    runtime_directory_listing listing;
    cJSON *names = cJSON_CreateArray();
    if (runtime_list_directory(path, &listing)) {
        for (size_t index = 0; index < listing.count; index++) {
            cJSON_AddItemToArray(names, cJSON_CreateString(listing.entries[index].name));
        }
        runtime_free_directory_listing(&listing);
    }
    free(path);
    char *text = cJSON_PrintUnformatted(names);
    cJSON_Delete(names);
    return text;
}

static void expect_listing(const char *rest, const char *expected_json, const char *label) {
    char *text = listing_of(rest);
    check_text(text, expected_json, label);
    cJSON_free(text);
}

static char *resolved_in_cache(const char *rest) {
    char *path = in_cache(rest);
    char *resolved = runtime_catalog_resolve_path(path);
    free(path);
    return resolved;
}

static void start_cache(const char *label) {
    snprintf(scratch, sizeof scratch, "%s/cache-%d-%s", RUNTIME_SCRATCH_PATH, (int)current_process_id(), label);
    start_empty_directory(scratch);
    snprintf(cache, sizeof cache, "%s/cache", scratch);
    snprintf(state, sizeof state, "%s/state", scratch);
}

static void finish_cache(void) {
    CHECK(runtime_remove_tree(scratch) == 1);
}

static void it_fetches_into_a_fresh_store_then_answers_cached_without_fetching(void) {
    start_cache("fresh");
    catalog_answer first = install("a", plugin_item(), "1.0.0", "linux-x64");
    CHECK(first.status == SPISOR_STATUS_SUCCESS);
    expect_member(first.tree, "fetched", "[\"plugin.json\",\"dist/index.js\"]", "fresh fetched");
    cJSON_Delete(first.tree);
    expect_file_text(cache, "bundles/a/1.0.0/dist/index.js", INDEX_JS, "fresh content");
    cJSON *item = plugin_item();
    remove_source("plugin.json");
    remove_source("index.js");
    catalog_answer again = install("a", item, "1.0.0", "linux-x64");
    CHECK(again.status == SPISOR_STATUS_SUCCESS);
    expect_member(again.tree, "fetched", "[]", "cached fetched");
    expect_member(again.tree, "cached", "[\"plugin.json\",\"dist/index.js\"]", "cached cached");
    cJSON_Delete(again.tree);
    finish_cache();
}

static void it_stops_a_download_serving_more_than_the_size_the_catalog_lists(void) {
    start_cache("oversized");
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "p.js", reference_serving("p.js", INDEX_JS "more", INDEX_JS "more", strlen(INDEX_JS)));
    expect_refused(install("a", item_of("plugin", "1.0.0", version_with_files(files)), "1.0.0", "linux-x64"),
                   "bytes", "oversized");
    expect_absent_in_cache("bundles/a/1.0.0/p.js", "oversized");
    expect_listing("bundles/a/1.0.0", "[]", "oversized leaves no partial file");
    finish_cache();
}

static cJSON *single_file_item(cJSON *reference) {
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "p.js", reference);
    return item_of("plugin", "1.0.0", version_with_files(files));
}

static void it_refetches_a_cached_file_whose_hash_no_longer_matches(void) {
    start_cache("corrupted");
    write_in(cache, "bundles/a/1.0.0/p.js", "corrupted");
    catalog_answer answer = install("a", single_file_item(reference_to("p.js", INDEX_JS)), "1.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_member(answer.tree, "fetched", "[\"p.js\"]", "refetched");
    cJSON_Delete(answer.tree);
    expect_file_text(cache, "bundles/a/1.0.0/p.js", INDEX_JS, "refetched content");
    finish_cache();
}

static void it_refuses_a_source_serving_other_bytes_naming_the_url_and_the_expected_hash(void) {
    start_cache("other-bytes");
    const char *other = "export default 1;;";
    cJSON *reference = reference_serving("p.js", INDEX_JS, other, strlen(other));
    char url[2048];
    snprintf(url, sizeof url, "%s", text_at(reference, "url"));
    char expected_sha256[RUNTIME_SHA256_HEX_SIZE];
    sha256_of_text(INDEX_JS, expected_sha256);
    catalog_answer answer = install("a", single_file_item(reference), "1.0.0", "linux-x64");
    expect_refusal(answer, expected_sha256, "other bytes hash");
    expect_refusal(answer, url, "other bytes url");
    expect_refusal(answer, "p.js", "other bytes file");
    cJSON_Delete(answer.tree);
    expect_absent_in_cache("bundles/a/1.0.0/p.js", "other bytes");
    expect_listing("bundles/a/1.0.0", "[]", "other bytes writes nothing");
    finish_cache();
}

static void it_refuses_a_fetched_file_whose_length_differs_from_the_size_even_when_the_hash_matches(void) {
    start_cache("length");
    cJSON *reference = reference_serving("p.js", INDEX_JS, INDEX_JS, strlen(INDEX_JS) + 1);
    expect_refused(install("a", single_file_item(reference), "1.0.0", "linux-x64"), "bytes", "length");
    expect_absent_in_cache("bundles/a/1.0.0/p.js", "length");
    expect_absent_in_cache("bundles/a/1.0.0/.installed.json", "length marker");
    finish_cache();
}

typedef struct concurrent_install {
    cJSON *item;
    catalog_answer answer;
} concurrent_install;

static void install_concurrently(concurrent_install *job) {
    job->answer = install("a", job->item, "1.0.0", "linux-x64");
}

#if defined(_WIN32)
static DWORD WINAPI installing_entry(LPVOID argument) {
    install_concurrently(argument);
    return 0;
}
#else
static void *installing_entry(void *argument) {
    install_concurrently(argument);
    return NULL;
}
#endif

static void it_lets_two_concurrent_stores_of_one_path_both_succeed(void) {
    start_cache("concurrent");
    concurrent_install jobs[2];
    jobs[0].item = single_file_item(reference_to("p.js", INDEX_JS));
    jobs[1].item = cJSON_Duplicate(jobs[0].item, 1);
#if defined(_WIN32)
    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, installing_entry, &jobs[0], 0, NULL);
    threads[1] = CreateThread(NULL, 0, installing_entry, &jobs[1], 0, NULL);
    CHECK(threads[0] != NULL && threads[1] != NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
#else
    pthread_t threads[2];
    CHECK(pthread_create(&threads[0], NULL, installing_entry, &jobs[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, installing_entry, &jobs[1]) == 0);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
#endif
    CHECK(jobs[0].answer.status == SPISOR_STATUS_SUCCESS);
    CHECK(jobs[1].answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(jobs[0].answer.tree);
    cJSON_Delete(jobs[1].answer.tree);
    expect_file_text(cache, "bundles/a/1.0.0/p.js", INDEX_JS, "concurrent content");
    expect_listing("bundles/a/1.0.0", "[\".installed.json\",\"p.js\"]", "concurrent leaves nothing else");
    finish_cache();
}

#if defined(_WIN32)
static HANDLE hold_without_delete_sharing(const char *path) {
    wchar_t wide[2048];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 2048) > 0);
    HANDLE held = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    return held;
}

static void it_refuses_when_its_rename_fails_and_the_destination_holds_other_bytes(void) {
    start_cache("held-other");
    char *destination = in_cache("bundles/a/1.0.0/p.js");
    write_file(destination, "other");
    HANDLE held = hold_without_delete_sharing(destination);
    expect_refused(install("a", single_file_item(reference_to("p.js", INDEX_JS)), "1.0.0", "linux-x64"),
                   "could not be written", "held with other bytes");
    CloseHandle(held);
    free(destination);
    expect_listing("bundles/a/1.0.0", "[\"p.js\"]", "held with other bytes leaves nothing else");
    finish_cache();
}
#endif

static void it_leaves_no_temporary_file_behind_even_when_the_rename_into_place_fails(void) {
    start_cache("occupied");
    char *occupied = in_cache("bundles/a/1.0.0/p.js/occupied");
    CHECK(spisor_make_directories(occupied) == 1);
    free(occupied);
    expect_refused(install("a", single_file_item(reference_to("p.js", INDEX_JS)), "1.0.0", "linux-x64"),
                   "could not be written", "occupied");
    expect_listing("bundles/a/1.0.0", "[\"p.js\"]", "occupied leaves no temporary file");
    finish_cache();
}

static void it_installs_a_plugin_s_files_into_its_bundle_version_directory(void) {
    start_cache("plugin");
    char *directory = resolved_in_cache("bundles/a/1.0.0");
    cJSON *expected = cJSON_CreateObject();
    cJSON_AddStringToObject(expected, "id", "a");
    cJSON_AddStringToObject(expected, "version", "1.0.0");
    cJSON_AddStringToObject(expected, "directory", directory);
    cJSON_AddItemToObject(expected, "fetched", parsed("[\"plugin.json\",\"dist/index.js\"]"));
    cJSON_AddItemToObject(expected, "cached", cJSON_CreateArray());
    char *expected_json = cJSON_PrintUnformatted(expected);
    cJSON_Delete(expected);
    expect_answer(install("a", plugin_item(), "1.0.0", "linux-x64"), expected_json, "plugin installed");
    cJSON_free(expected_json);
    free(directory);
    expect_file_text(cache, "bundles/a/1.0.0/dist/index.js", INDEX_JS, "plugin content");
    finish_cache();
}

static void it_marks_a_version_complete_last_naming_every_file_its_hash_and_its_size(void) {
    start_cache("marker");
    cJSON *item = plugin_item();
    char plugin_sha256[RUNTIME_SHA256_HEX_SIZE];
    char index_sha256[RUNTIME_SHA256_HEX_SIZE];
    sha256_of_text(PLUGIN_JSON, plugin_sha256);
    sha256_of_text(INDEX_JS, index_sha256);
    catalog_answer answer = install("a", item, "1.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    char expected[768];
    snprintf(expected, sizeof expected,
             "{\n  \"files\": {\n    \"plugin.json\": \"%s\",\n    \"dist/index.js\": \"%s\"\n  },\n  \"sizes\": {\n"
             "    \"plugin.json\": %zu,\n    \"dist/index.js\": %zu\n  }\n}\n",
             plugin_sha256, index_sha256, strlen(PLUGIN_JSON), strlen(INDEX_JS));
    expect_file_text(cache, "bundles/a/1.0.0/.installed.json", expected, "marker text");
    finish_cache();
}

static char *read_text_in(const char *rest) {
    char *path = in_cache(rest);
    char *text = read_text(path);
    free(path);
    return text;
}

/** A version a sweep removed between installed listing it and its check is gone, not damaged: no entry, so none without a detail. */
static void it_reports_nothing_for_a_version_whose_marker_is_gone(void) {
    start_cache("damage-absent");
    char *directory = in_cache("bundles/a/1.0.0");
    CHECK(spisor_make_directories(directory) == 1);
    int out_of_memory = 1;
    cJSON *damage = runtime_cache_version_damage(directory, &out_of_memory);
    CHECK(damage == NULL);
    CHECK(out_of_memory == 0);
    cJSON_Delete(damage);
    free(directory);
    finish_cache();
}

static cJSON *damaged_in_cache(void) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    catalog_answer answer = call_catalog(runtime_cache_damaged, argument);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON *damaged = cJSON_DetachItemFromObjectCaseSensitive(answer.tree, "damaged");
    cJSON_Delete(answer.tree);
    return damaged;
}

static void expect_one_damaged(const char *part, const char *label) {
    cJSON *damaged = damaged_in_cache();
    const cJSON *first = cJSON_GetArrayItem(damaged, 0);
    CHECK(cJSON_GetArraySize(damaged) == 1);
    check_text(text_at(first, "id"), "a", label);
    check_text(text_at(first, "version"), "1.0.0", label);
    const char *detail = text_at(first, "detail");
    if (detail == NULL || strstr(detail, part) == NULL) {
        fprintf(stderr, "%s: expected a detail naming \"%s\", got \"%s\"\n", label, part, detail == NULL ? "(none)" : detail);
        check_failures++;
    }
    CHECK(text_at(first, "fix") != NULL);
    cJSON_Delete(damaged);
}

/** What status reports: a same-size change found by its hash, and a marker recording no sizes found as a start would refuse it. */
static void it_reports_each_installed_version_a_start_would_refuse_or_whose_hash_changed(void) {
    start_cache("damaged");
    catalog_answer answer = install("a", plugin_item(), "1.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    cJSON *none = damaged_in_cache();
    CHECK(cJSON_IsArray(none) && cJSON_GetArraySize(none) == 0);
    cJSON_Delete(none);
    char *original = read_text_in("bundles/a/1.0.0/dist/index.js");
    char *changed = malloc(strlen(original) + 1);
    strcpy(changed, original);
    changed[0] = changed[0] == 'x' ? 'y' : 'x';
    write_in(cache, "bundles/a/1.0.0/dist/index.js", changed);
    expect_one_damaged("has SHA-256", "same size");
    write_in(cache, "bundles/a/1.0.0/dist/index.js", original);
    char *marker = read_text_in("bundles/a/1.0.0/.installed.json");
    cJSON *parsed_marker = spisor_parse_json((const uint8_t *)marker, strlen(marker));
    cJSON_DeleteItemFromObjectCaseSensitive(parsed_marker, "sizes");
    char *stripped = cJSON_PrintUnformatted(parsed_marker);
    write_in(cache, "bundles/a/1.0.0/.installed.json", stripped);
    expect_one_damaged("records no size for", "no sizes");
    cJSON_free(stripped);
    cJSON_Delete(parsed_marker);
    free(marker);
    free(changed);
    free(original);
    finish_cache();
}

static void it_stores_files_in_the_order_javascript_enumerates_their_keys(void) {
    start_cache("key-order");
    cJSON *entry = cJSON_CreateObject();
    cJSON *shared = cJSON_AddObjectToObject(entry, "files");
    cJSON_AddItemToObject(shared, "b.js", reference_to("b.js", "b"));
    cJSON_AddItemToObject(shared, "1", reference_to("one", "1"));
    cJSON *own = cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(entry, "platforms"), "linux-x64"),
                                         "files");
    cJSON_AddItemToObject(own, "a.js", reference_to("a.js", "a"));
    cJSON_AddItemToObject(own, "0", reference_to("zero", "0"));
    catalog_answer answer = install("liba", item_of("library", "2.0.0", entry), "2.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_member(answer.tree, "fetched", "[\"0\",\"1\",\"b.js\",\"a.js\"]", "key order");
    cJSON_Delete(answer.tree);
    finish_cache();
}

static void it_leaves_no_runnable_version_when_the_second_of_two_files_is_refused(void) {
    start_cache("second");
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "plugin.json", reference_to("plugin.json", PLUGIN_JSON));
    cJSON_AddItemToObject(files, "dist/index.js",
                          reference_serving("index.js", INDEX_JS, "export default 1;", strlen("export default 1;")));
    expect_refused(install("a", item_of("plugin", "1.0.0", version_with_files(files)), "1.0.0", "linux-x64"),
                   "dist/index.js", "second refused");
    char *listing = listing_of("bundles/a/1.0.0");
    CHECK(listing != NULL && strstr(listing, ".installed.json") == NULL);
    cJSON_free(listing);
    char *directory = in_cache("bundles/a/1.0.0");
    catalog_answer check = check_directory(directory);
    expect_answer(check, "{\"kind\":\"absent\"}", "second leaves the version absent");
    free(directory);
    finish_cache();
}

static void it_refuses_as_ambiguous_a_file_listed_both_for_every_platform_and_for_this_one(void) {
    start_cache("ambiguous");
    cJSON *entry = cJSON_CreateObject();
    cJSON *shared = cJSON_AddObjectToObject(entry, "files");
    cJSON_AddItemToObject(shared, "liba.so", reference_to("liba.so", LIBRARY_SO));
    cJSON *platforms = cJSON_AddObjectToObject(entry, "platforms");
    cJSON *files = cJSON_AddObjectToObject(cJSON_AddObjectToObject(platforms, "linux-x64"), "files");
    cJSON_AddItemToObject(files, "liba.so", reference_to("liba.so", LIBRARY_SO));
    expect_refused(install("liba", item_of("library", "2.0.0", entry), "2.0.0", "linux-x64"),
                   "file liba.so is listed both for every platform and for linux-x64", "ambiguous");
    expect_absent_in_cache("libraries", "ambiguous");
    finish_cache();
}

static void it_refuses_a_file_that_would_take_the_completion_marker_s_name(void) {
    start_cache("shadowing");
    const char *names[2] = {".installed.json", ".INSTALLED.json"};
    for (size_t index = 0; index < 2; index++) {
        cJSON *files = cJSON_CreateObject();
        cJSON_AddItemToObject(files, names[index], reference_to("index.js", INDEX_JS));
        expect_refused(install("a", item_of("plugin", "1.0.0", version_with_files(files)), "1.0.0", "linux-x64"),
                       names[index], "shadowing");
        expect_absent_in_cache("bundles", "shadowing");
    }
    finish_cache();
}

static void it_marks_a_version_of_no_files_complete_making_its_directory(void) {
    start_cache("no-files");
    catalog_answer answer = install("a", item_of("plugin", "1.0.0", version_with_files(cJSON_CreateObject())), "1.0.0",
                                    "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    expect_file_text(cache, "bundles/a/1.0.0/.installed.json", "{\n  \"files\": {},\n  \"sizes\": {}\n}\n", "no files marker");
    finish_cache();
}

static void it_answers_cached_for_every_file_on_a_second_install(void) {
    start_cache("second-install");
    catalog_answer first = install("a", plugin_item(), "1.0.0", "linux-x64");
    CHECK(first.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(first.tree);
    catalog_answer again = install("a", plugin_item(), "1.0.0", "linux-x64");
    CHECK(again.status == SPISOR_STATUS_SUCCESS);
    expect_member(again.tree, "fetched", "[]", "second install fetched");
    expect_member(again.tree, "cached", "[\"plugin.json\",\"dist/index.js\"]", "second install cached");
    cJSON_Delete(again.tree);
    finish_cache();
}

static void it_installs_a_library_s_files_for_the_platform_into_its_library_directory(void) {
    start_cache("library");
    catalog_answer answer = install("liba", linux_only_library(), "2.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    expect_listing("libraries/liba/2.0.0/linux-x64", "[\".installed.json\",\"liba.so\"]", "library listing");
    finish_cache();
}

static void it_refuses_an_item_not_built_for_this_platform_with_the_fix(void) {
    start_cache("not-built");
    catalog_answer answer = install("liba", linux_only_library(), "2.0.0", "win32-x64");
    CHECK(answer.status == SPISOR_STATUS_FAILURE);
    check_text(text_at(answer.tree, "message"), "not built for win32-x64", "not built message");
    check_text(text_at(answer.tree, "fix"), "use a platform the item lists, or ask its publisher for a win32-x64 build",
               "not built fix");
    cJSON_Delete(answer.tree);
    expect_absent_in_cache("libraries", "not built");
    finish_cache();
}

static void it_refuses_a_version_the_item_does_not_list(void) {
    start_cache("unlisted");
    expect_refused(install("a", plugin_item(), "9.9.9", "linux-x64"), "the catalog lists no version 9.9.9", "unlisted");
    expect_refused(install("a", plugin_item(), "constructor", "linux-x64"), "no version constructor", "inherited name");
    finish_cache();
}

static void it_refuses_a_file_whose_destination_escapes_the_version_directory(void) {
    start_cache("escaping");
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "index.js", reference_to("index.js", INDEX_JS));
    cJSON_AddItemToObject(files, "../../escaped.js", reference_to("index.js", INDEX_JS));
    expect_refused(install("a", item_of("plugin", "1.0.0", version_with_files(files)), "1.0.0", "linux-x64"),
                   "file ../../escaped.js would be written outside", "escaping");
    expect_absent_in_cache("bundles", "escaping");
    finish_cache();
}

static void it_refuses_an_id_or_a_platform_key_that_would_climb_out_of_the_cache(void) {
    start_cache("climbing");
    expect_refused(install("../a", plugin_item(), "1.0.0", "linux-x64"), "would be installed outside", "climbing id");
    expect_refused(install("liba", linux_only_library(), "2.0.0", "../x"), "a platform key is <os>-<arch>",
                   "climbing platform");
    expect_absent_in_cache("bundles", "climbing");
    expect_absent_in_cache("libraries", "climbing");
    finish_cache();
}

static void it_refuses_a_mismatched_file_naming_the_item_the_file_and_the_expected_hash(void) {
    start_cache("lying");
    char lie[sizeof PLUGIN_JSON];
    memset(lie, 'x', sizeof lie - 1);
    lie[sizeof lie - 1] = 0;
    cJSON *files = cJSON_CreateObject();
    cJSON_AddItemToObject(files, "plugin.json", reference_serving("plugin.json", PLUGIN_JSON, lie, strlen(lie)));
    cJSON_AddItemToObject(files, "dist/index.js", reference_serving("index.js", INDEX_JS, lie, strlen(INDEX_JS)));
    char expected_sha256[RUNTIME_SHA256_HEX_SIZE];
    sha256_of_text(PLUGIN_JSON, expected_sha256);
    catalog_answer answer = install("a", item_of("plugin", "1.0.0", version_with_files(files)), "1.0.0", "linux-x64");
    expect_refusal(answer, "a@1.0.0", "lying item");
    expect_refusal(answer, "file plugin.json", "lying file");
    expect_refusal(answer, expected_sha256, "lying hash");
    cJSON_Delete(answer.tree);
    finish_cache();
}

/* The brief's interrupted install */

static void it_completes_an_interrupted_install_on_the_next_install(void) {
    start_cache("interrupted");
    char *plugin_destination = in_cache("bundles/a/1.0.0/plugin.json");
    write_file(plugin_destination, PLUGIN_JSON);
    free(plugin_destination);
    char *directory = in_cache("bundles/a/1.0.0");
    expect_answer(check_directory(directory), "{\"kind\":\"absent\"}", "interrupted before");
    catalog_answer answer = install("a", plugin_item(), "1.0.0", "linux-x64");
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    expect_member(answer.tree, "cached", "[\"plugin.json\"]", "interrupted cached");
    expect_member(answer.tree, "fetched", "[\"dist/index.js\"]", "interrupted fetched");
    cJSON_Delete(answer.tree);
    expect_answer(check_directory(directory), "{\"kind\":\"intact\"}", "interrupted after");
    free(directory);
    finish_cache();
}

static char *marked_bundle(const char *id, const char *version, int marked) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/%s/%s", id, version);
    char *directory = in_cache(rest);
    CHECK(spisor_make_directories(directory) == 1);
    const char *names[2] = {"plugin.json", "index.js"};
    const char *contents[2] = {PLUGIN_JSON, INDEX_JS};
    cJSON *marker = cJSON_CreateObject();
    cJSON *hashes = cJSON_AddObjectToObject(marker, "files");
    for (size_t index = 0; index < 2; index++) {
        write_in(directory, names[index], contents[index]);
        char sha256[RUNTIME_SHA256_HEX_SIZE];
        sha256_of_text(contents[index], sha256);
        cJSON_AddStringToObject(hashes, names[index], sha256);
    }
    if (marked) {
        char *text = cJSON_PrintUnformatted(marker);
        write_in(directory, ".installed.json", text);
        cJSON_free(text);
    }
    cJSON_Delete(marker);
    return directory;
}

static void expect_damaged(catalog_answer answer, const char *file, const char *fragment, const char *label) {
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "kind"), "damaged", label);
    CHECK(text_at(answer.tree, "fix") != NULL);
    if (file != NULL) {
        check_text(text_at(answer.tree, "file"), file, label);
    }
    const char *detail = text_at(answer.tree, "detail");
    if (detail == NULL || strstr(detail, fragment) == NULL) {
        fprintf(stderr, "%s: expected a detail naming \"%s\", got \"%s\"\n", label, fragment, detail == NULL ? "(none)" : detail);
        check_failures++;
    }
    cJSON_Delete(answer.tree);
}

static void it_answers_intact_for_a_version_whose_files_carry_their_recorded_hashes(void) {
    start_cache("intact");
    char *directory = marked_bundle("a", "2.0.0", 1);
    expect_answer(check_directory(directory), "{\"kind\":\"intact\"}", "intact");
    free(directory);
    finish_cache();
}

static void it_answers_absent_for_a_version_that_is_not_installed(void) {
    start_cache("absent");
    char *directory = in_cache("bundles/a/2.0.0");
    expect_answer(check_directory(directory), "{\"kind\":\"absent\"}", "not installed");
    free(directory);
    finish_cache();
}

static void it_answers_absent_for_a_version_with_no_completion_marker(void) {
    start_cache("unmarked");
    char *directory = marked_bundle("a", "1.0.0", 0);
    expect_answer(check_directory(directory), "{\"kind\":\"absent\"}", "unmarked");
    free(directory);
    finish_cache();
}

static void it_answers_damaged_for_a_corrupted_file_naming_it(void) {
    start_cache("damaged");
    char *directory = marked_bundle("a", "1.0.0", 1);
    write_in(directory, "index.js", "export default {}!");
    expect_damaged(check_directory(directory), "index.js", "file index.js of", "corrupted");
    free(directory);
    finish_cache();
}

static void it_answers_damaged_for_a_missing_file(void) {
    start_cache("missing");
    char *directory = marked_bundle("a", "1.0.0", 1);
    char *file = joined(directory, "index.js");
    CHECK(runtime_remove_file_entry(file) == RUNTIME_ENTRY_DONE);
    free(file);
    expect_damaged(check_directory(directory), "index.js", "is missing", "missing");
    free(directory);
    finish_cache();
}

static void it_answers_damaged_for_a_completion_marker_that_is_not_readable(void) {
    start_cache("unreadable");
    char *directory = marked_bundle("a", "1.0.0", 1);
    write_in(directory, ".installed.json", "{");
    expect_damaged(check_directory(directory), NULL, "is not readable", "unreadable marker");
    write_in(directory, ".installed.json", "{\"files\":{\"index.js\":\"ABC\"}}");
    expect_damaged(check_directory(directory), NULL, "is not readable", "marker with a bad hash");
    free(directory);
    finish_cache();
}

static void it_answers_damaged_for_a_marker_that_names_a_file_outside_its_version_directory(void) {
    start_cache("outside");
    char *directory = marked_bundle("a", "1.0.0", 1);
    write_in(directory, ".installed.json",
             "{\"files\":{\"../../outside.js\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}");
    expect_damaged(check_directory(directory), "../../outside.js", "which is not a file of", "outside");
    free(directory);
    finish_cache();
}

static void make_version(const char *rest, const char *marker) {
    char *directory = in_cache(rest);
    CHECK(spisor_make_directories(directory) == 1);
    if (marker != NULL) {
        write_in(directory, ".installed.json", marker);
    }
    free(directory);
}

static void it_answers_each_id_s_versions_that_carry_a_readable_completion_marker(void) {
    start_cache("installed");
    make_version("bundles/a/1.0.0", "{\"files\":{}}");
    make_version("bundles/a/2.0.0", NULL);
    make_version("bundles/a/3.0.0", "{");
    make_version("bundles/b/1.0.0", "{\"files\":{\"plugin.json\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}");
    char *present = in_cache("bundles/b/1.0.0");
    write_in(present, "plugin.json", "{}");
    free(present);
    make_version("libraries/runtime/0.1.0/linux-x64", "{\"files\":{}}");
    make_version("libraries/runtime/0.2.0/win32-x64", "{\"files\":{}}");
    make_version("libraries/runtime/0.3.0/linux-x64", NULL);
    expect_answer(installed("linux-x64"), "{\"bundles\":{\"a\":[\"1.0.0\"],\"b\":[\"1.0.0\"]},\"libraries\":{\"runtime\":[\"0.1.0\"]}}",
                  "installed");
    finish_cache();
}

/** A sweep racing an install can leave a marker naming files it already deleted; no caller may run that version. */
static void it_leaves_out_a_version_whose_marker_names_a_file_that_is_missing(void) {
    start_cache("installed-missing");
    make_version("bundles/a/1.0.0", "{\"files\":{\"plugin.json\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}");
    make_version("libraries/runtime/0.1.0/linux-x64",
                 "{\"files\":{\"runtime.so\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}");
    expect_answer(installed("linux-x64"), "{\"bundles\":{},\"libraries\":{}}", "missing files");
    finish_cache();
}

static void it_answers_nothing_for_a_cache_with_no_bundles_folder(void) {
    start_cache("installed-empty");
    expect_answer(installed(NULL), "{\"bundles\":{},\"libraries\":{}}", "installed empty");
    finish_cache();
}

static void installed_bundle(const char *id, const char *version, int marked) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/%s/%s", id, version);
    char *directory = in_cache(rest);
    write_in(directory, "plugin.json", "{}");
    if (marked) {
        write_in(directory, ".installed.json", "{\"files\":{}}");
    }
    free(directory);
}

static void installed_library(const char *id, const char *version, const char *platform, int marked) {
    char rest[256];
    snprintf(rest, sizeof rest, "libraries/%s/%s/%s", id, version, platform);
    char *directory = in_cache(rest);
    write_in(directory, "library.json", "{}");
    if (marked) {
        write_in(directory, ".installed.json", "{\"files\":{}}");
    }
    free(directory);
}

static int bundle_present(const char *id, const char *version) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/%s/%s", id, version);
    return exists_in_cache(rest);
}

static int library_present(const char *id, const char *version, const char *platform) {
    char rest[256];
    snprintf(rest, sizeof rest, "libraries/%s/%s/%s", id, version, platform);
    return exists_in_cache(rest);
}

static void record_references(const char *host, const char *json) {
    char rest[256];
    snprintf(rest, sizeof rest, "bundles/%s.json", host);
    write_in(state, rest, json);
}

static void set_modified(const char *path, double milliseconds) {
#if defined(_WIN32)
    wchar_t wide[2048];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 2048) > 0);
    HANDLE handle = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    ULONGLONG ticks = (ULONGLONG)(milliseconds * 10000.0) + 116444736000000000ULL;
    FILETIME time = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    CHECK(SetFileTime(handle, NULL, &time, &time) != 0);
    CloseHandle(handle);
#else
    struct timespec times[2];
    times[0].tv_sec = (time_t)(milliseconds / 1000.0);
    times[0].tv_nsec = 0;
    times[1] = times[0];
    CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
#endif
}

static double now_milliseconds(void) {
#if defined(_WIN32)
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULONGLONG ticks = ((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return (double)(ticks - 116444736000000000ULL) / 10000.0;
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
#endif
}

/** Dates path and everything under it hours into the past. */
static void age_tree(const char *path, double milliseconds) {
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(path, &listing) == 1);
    for (size_t index = 0; index < listing.count; index++) {
        char *child = joined(path, listing.entries[index].name);
        if (listing.entries[index].is_directory) {
            age_tree(child, milliseconds);
        } else {
            set_modified(child, milliseconds);
        }
        free(child);
    }
    runtime_free_directory_listing(&listing);
    set_modified(path, milliseconds);
}

static void age_in_cache(const char *rest, double hours) {
    char *path = in_cache(rest);
    age_tree(path, now_milliseconds() - hours * HOUR_MILLISECONDS);
    free(path);
}

static void expect_removed(const char *expected_json, const char *label) {
    catalog_answer answer = sweep();
    char *wrapped = malloc(strlen(expected_json) + 16);
    sprintf(wrapped, "{\"removed\":%s}", expected_json);
    expect_answer(answer, wrapped, label);
    free(wrapped);
}

static void it_keeps_every_version_a_reference_file_names_and_the_newest_installed(void) {
    start_cache("sweep-referenced");
    const char *versions[4] = {"1.0.0", "1.1.0", "1.2.0", "2.0.0"};
    for (size_t index = 0; index < 4; index++) {
        installed_bundle("a", versions[index], 1);
    }
    record_references("claude-code", "{\"run\":{\"a\":\"1.1.0\"},\"next\":{}}");
    expect_removed("[\"a@1.0.0\",\"a@1.2.0\"]", "referenced");
    CHECK(!bundle_present("a", "1.0.0") && bundle_present("a", "1.1.0") && !bundle_present("a", "1.2.0") &&
          bundle_present("a", "2.0.0"));
    finish_cache();
}

static void it_sweeps_a_marked_version_whose_files_are_gone(void) {
    start_cache("sweep-files-gone");
    make_version("bundles/a/1.0.0", "{\"files\":{\"plugin.json\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}");
    installed_bundle("a", "2.0.0", 1);
    expect_removed("[\"a@1.0.0\"]", "files gone");
    CHECK(!bundle_present("a", "1.0.0") && bundle_present("a", "2.0.0"));
    finish_cache();
}

static void it_keeps_the_versions_two_hosts_reference_files_name(void) {
    start_cache("sweep-two-hosts");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    installed_bundle("a", "3.0.0", 1);
    record_references("claude-code", "{\"run\":{\"a\":\"1.0.0\"},\"next\":{}}");
    record_references("opencode", "{\"run\":{\"a\":\"2.0.0\"},\"next\":{}}");
    expect_removed("[]", "two hosts");
    finish_cache();
}

static void it_keeps_a_version_a_host_scheduled_for_its_next_start(void) {
    start_cache("sweep-next");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    installed_bundle("a", "3.0.0", 1);
    record_references("claude-code", "{\"run\":{\"a\":\"2.0.0\"},\"next\":{\"a\":\"1.0.0\"}}");
    expect_removed("[]", "next");
    finish_cache();
}

static void it_keeps_the_newest_installed_version_of_an_id_no_host_references(void) {
    start_cache("sweep-rollback");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "1.10.0", 1);
    expect_removed("[\"a@1.0.0\"]", "rollback");
    CHECK(bundle_present("a", "1.10.0"));
    finish_cache();
}

static void it_leaves_a_version_with_no_completion_marker(void) {
    start_cache("sweep-unmarked");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 0);
    installed_bundle("a", "0.5.0", 0);
    expect_removed("[]", "unmarked");
    CHECK(bundle_present("a", "0.5.0"));
    finish_cache();
}

static void it_does_not_follow_a_reference_file_s_names_out_of_the_bundles_folder(void) {
    start_cache("sweep-escape");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    char *outside = joined(scratch, "outside");
    CHECK(spisor_make_directories(outside) == 1);
    record_references("claude-code", "{\"run\":{\"..\":\"..\",\"a\":\"../../../outside\"},\"next\":{\"a\":\"../..\"}}");
    expect_removed("[\"a@1.0.0\"]", "escape");
    CHECK(spisor_file_exists(outside));
    free(outside);
    finish_cache();
}

static void it_answers_nothing_removed_for_a_cache_with_no_bundles(void) {
    start_cache("sweep-empty");
    expect_removed("[]", "empty");
    finish_cache();
}

static void it_sweeps_library_versions_per_platform_like_bundles(void) {
    start_cache("sweep-libraries");
    const char *versions[4] = {"0.1.0", "0.2.0", "0.3.0", "0.4.0"};
    for (size_t index = 0; index < 4; index++) {
        installed_library("runtime", versions[index], "linux-x64", 1);
    }
    installed_library("runtime", "0.1.0", "win32-x64", 1);
    record_references("forebay-host", "{\"run\":{\"runtime\":\"0.2.0\"},\"next\":{}}");
    expect_removed("[\"runtime@0.1.0 for linux-x64\",\"runtime@0.3.0 for linux-x64\"]", "libraries");
    CHECK(!library_present("runtime", "0.1.0", "linux-x64") && library_present("runtime", "0.2.0", "linux-x64") &&
          !library_present("runtime", "0.3.0", "linux-x64") && library_present("runtime", "0.4.0", "linux-x64"));
    CHECK(library_present("runtime", "0.1.0", "win32-x64"));
    CHECK(!exists_in_cache("libraries/runtime/0.3.0"));
    finish_cache();
}

static void it_leaves_a_library_version_with_no_completion_marker_alone(void) {
    start_cache("sweep-library-unmarked");
    installed_library("koffi", "1.0.0", "linux-x64", 0);
    installed_library("koffi", "2.0.0", "linux-x64", 1);
    expect_removed("[]", "library unmarked");
    finish_cache();
}

static void it_removes_an_unmarked_bundle_version_whose_newest_file_is_more_than_a_day_old(void) {
    start_cache("sweep-abandoned");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 0);
    age_in_cache("bundles/a/2.0.0", 25);
    expect_removed("[\"a@2.0.0\"]", "abandoned");
    CHECK(!bundle_present("a", "2.0.0") && bundle_present("a", "1.0.0"));
    finish_cache();
}

static void it_keeps_an_unmarked_bundle_version_while_any_one_of_its_files_is_recent(void) {
    start_cache("sweep-arriving");
    installed_bundle("a", "2.0.0", 0);
    age_in_cache("bundles/a/2.0.0", 25);
    char *directory = in_cache("bundles/a/2.0.0");
    write_in(directory, "index.js", "still arriving");
    set_modified(directory, now_milliseconds() - 25 * HOUR_MILLISECONDS);
    free(directory);
    expect_removed("[]", "arriving");
    finish_cache();
}

static void it_keeps_an_old_unmarked_bundle_version_a_host_references(void) {
    start_cache("sweep-referenced-abandoned");
    installed_bundle("a", "2.0.0", 0);
    age_in_cache("bundles/a/2.0.0", 25);
    record_references("claude-code", "{\"run\":{},\"next\":{\"a\":\"2.0.0\"}}");
    expect_removed("[]", "referenced abandoned");
    finish_cache();
}

static void it_keys_an_abandoned_id_folder_by_its_id_when_it_checks_references_and_names_what_it_removes(void) {
    start_cache("sweep-id-abandoned");
    installed_bundle("github+5", "2.0.0", 0);
    age_in_cache("bundles/github+5/2.0.0", 25);
    record_references("claude-code", "{\"run\":{},\"next\":{\"github:5\":\"2.0.0\"}}");
    expect_removed("[]", "referenced id key");
    record_references("claude-code", "{\"run\":{},\"next\":{}}");
    expect_removed("[\"github:5@2.0.0\"]", "unreferenced id key");
    finish_cache();
}

static void it_removes_an_old_unmarked_library_version_for_one_platform(void) {
    start_cache("sweep-library-abandoned");
    installed_library("koffi", "2.0.0", "linux-x64", 1);
    installed_library("koffi", "1.0.0", "linux-x64", 0);
    age_in_cache("libraries/koffi/1.0.0/linux-x64", 25);
    installed_library("koffi", "1.0.0", "win32-x64", 1);
    expect_removed("[\"koffi@1.0.0 for linux-x64\"]", "library abandoned");
    CHECK(!library_present("koffi", "1.0.0", "linux-x64") && library_present("koffi", "1.0.0", "win32-x64"));
    finish_cache();
}

static void it_skips_a_reference_file_that_is_not_two_maps_of_versions(void) {
    start_cache("sweep-broken-references");
    const char *versions[5] = {"1.0.0", "2.0.0", "3.0.0", "4.0.0", "5.0.0"};
    for (size_t index = 0; index < 5; index++) {
        installed_bundle("a", versions[index], 1);
    }
    record_references("claude-code", "{\"run\":{\"a\":\"1.0.0\"},\"next\":{}}");
    record_references("opencode", "{\"run\":{},\"next\":{\"a\":\"2.0.0\"}}");
    record_references("broken", "{");
    record_references("flat", "{\"a\":\"3.0.0\"}");
    record_references("wrong", "{\"run\":{\"a\":1,\"b\":\"3.0.0\"},\"next\":{\"a\":\"4.0.0\"}}");
    char *folder = joined(state, "bundles/folder.json");
    CHECK(spisor_make_directories(folder) == 1);
    free(folder);
    expect_removed("[\"a@3.0.0\",\"a@4.0.0\"]", "broken references");
    finish_cache();
}

static void it_keeps_what_a_reference_file_keeps_for_rollback(void) {
    start_cache("sweep-kept-rollback");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    installed_bundle("a", "3.0.0", 1);
    record_references("claude-code", "{\"run\":{\"a\":\"2.0.0\"},\"next\":{},\"rollback\":{\"a\":\"1.0.0\"}}");
    expect_removed("[]", "kept rollback");
    finish_cache();
}

static void it_keeps_what_a_boot_hint_names_for_the_next_start(void) {
    start_cache("sweep-hints");
    installed_bundle("forebay-host", "1.0.0", 1);
    installed_bundle("forebay-host", "2.0.0", 1);
    installed_bundle("forebay-host", "3.0.0", 1);
    installed_library("runtime", "0.1.0", "linux-x64", 1);
    installed_library("runtime", "0.2.0", "linux-x64", 1);
    installed_library("koffi", "1.0.0", "linux-x64", 1);
    installed_library("koffi", "2.0.0", "linux-x64", 1);
    write_in(cache, "hosts/forebay-host.json", "{\"libraries\":{\"runtime\":\"0.1.0\",\"koffi\":\"1.0.0\",\"bad\":2},\"host\":\"1.0.0\"}");
    write_in(cache, "hosts/broken.json", "{");
    write_in(cache, "hosts/elsewhere.txt", "{\"libraries\":{},\"host\":\"2.0.0\"}");
    expect_removed("[\"forebay-host@2.0.0\"]", "hints");
    CHECK(library_present("runtime", "0.1.0", "linux-x64") && library_present("koffi", "1.0.0", "linux-x64"));
    finish_cache();
}

static void write_run_record(const char *host, const char *name, long pid, double alive_at, const char *run) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/runs/%s/%s", host, name);
    char text[512];
    snprintf(text, sizeof text, "{\"pid\":%ld,\"aliveAt\":%.0f,\"checkMilliseconds\":5000,\"run\":%s}\n", pid, alive_at, run);
    write_in(state, rest, text);
}

static int run_record_present(const char *host, const char *name) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/runs/%s/%s", host, name);
    char *path = joined(state, rest);
    int present = spisor_file_exists(path);
    free(path);
    return present;
}

#if defined(_WIN32)
typedef struct dead_process {
    long pid;
    HANDLE handle;
} dead_process;

static dead_process exited_process(void) {
    dead_process dead = {0, NULL};
    wchar_t command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW startup;
    PROCESS_INFORMATION information;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    CHECK(CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &information) != 0);
    WaitForSingleObject(information.hProcess, INFINITE);
    CloseHandle(information.hThread);
    dead.pid = (long)information.dwProcessId;
    dead.handle = information.hProcess;
    return dead;
}

static void release_process(dead_process dead) {
    CloseHandle(dead.handle);
}
#else
typedef struct dead_process {
    long pid;
} dead_process;

static dead_process exited_process(void) {
    dead_process dead = {0};
    pid_t child = fork();
    if (child == 0) {
        _exit(0);
    }
    CHECK(child > 0);
    waitpid(child, NULL, 0);
    dead.pid = (long)child;
    return dead;
}

static void release_process(dead_process dead) {
    (void)dead;
}
#endif

static void it_prunes_a_run_record_whose_process_is_gone_and_its_versions_with_it(void) {
    start_cache("sweep-dead-run");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    dead_process dead = exited_process();
    write_run_record("fixture-loader", "dead.json", dead.pid, now_milliseconds(), "{\"a\":\"1.0.0\"}");
    expect_removed("[\"a@1.0.0\"]", "dead run");
    CHECK(!run_record_present("fixture-loader", "dead.json"));
    release_process(dead);
    finish_cache();
}

static void it_prunes_a_run_record_whose_process_stopped_stamping(void) {
    start_cache("sweep-stale-run");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    write_run_record("fixture-loader", "stale.json", (long)current_process_id(), now_milliseconds() - 60000.0,
                     "{\"a\":\"1.0.0\"}");
    write_in(state, "bundles/runs/fixture-loader/unreadable.json", "{");
    expect_removed("[\"a@1.0.0\"]", "stale run");
    CHECK(!run_record_present("fixture-loader", "stale.json"));
    CHECK(!run_record_present("fixture-loader", "unreadable.json"));
    finish_cache();
}

static void it_keeps_what_a_live_start_s_run_record_names(void) {
    start_cache("sweep-live-run");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    write_run_record("fixture-loader", "live.json", (long)current_process_id(), now_milliseconds(), "{\"a\":\"1.0.0\"}");
    expect_removed("[]", "live run");
    CHECK(run_record_present("fixture-loader", "live.json"));
    finish_cache();
}

/* The registry lock */

typedef struct sweeping {
    volatile int finished;
    catalog_answer answer;
} sweeping;

static void sweep_in_background(sweeping *job) {
    job->answer = sweep();
    job->finished = 1;
}

#if defined(_WIN32)
static DWORD WINAPI sweeping_entry(LPVOID argument) {
    sweep_in_background(argument);
    return 0;
}

static void pause_milliseconds(unsigned milliseconds) {
    Sleep(milliseconds);
}
#else
static void *sweeping_entry(void *argument) {
    sweep_in_background(argument);
    return NULL;
}

static void pause_milliseconds(unsigned milliseconds) {
    struct timespec delay = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
#endif

static void it_sweeps_only_while_it_holds_the_registry_lock(void) {
    start_cache("sweep-lock");
    installed_bundle("a", "1.0.0", 1);
    installed_bundle("a", "2.0.0", 1);
    spisor_lock *guard = runtime_hosts_lock(state);
    CHECK(guard != NULL);
    sweeping job;
    memset(&job, 0, sizeof job);
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, sweeping_entry, &job, 0, NULL);
    CHECK(thread != NULL);
#else
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, sweeping_entry, &job) == 0);
#endif
    pause_milliseconds(300);
    CHECK(!job.finished);
    CHECK(bundle_present("a", "1.0.0"));
    spisor_unlock(guard);
#if defined(_WIN32)
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    expect_answer(job.answer, "{\"removed\":[\"a@1.0.0\"]}", "after the lock");
    finish_cache();
}

static void it_stores_checks_and_sweeps_under_a_path_over_260_characters(void) {
    start_cache("deep");
    char deep[1300];
    snprintf(deep, sizeof deep, "%s", scratch);
    for (int segment = 0; segment < 5; segment++) {
        size_t length = strlen(deep);
        snprintf(deep + length, sizeof deep - length, "/%s%d", "a-folder-name-long-enough-to-push-the-path-past-the-limit-", segment);
    }
    snprintf(cache, sizeof cache, "%s/cache", deep);
    CHECK(strlen(cache) > 300);
    catalog_answer first = install("a", plugin_item(), "1.0.0", "linux-x64");
    if (first.status != SPISOR_STATUS_SUCCESS) {
        fprintf(stderr, "deep install refused: %s\n", text_at(first.tree, "message"));
        check_failures++;
    }
    cJSON_Delete(first.tree);
    cJSON *newer = plugin_item();
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(newer, "versions");
    cJSON_AddItemToObject(versions, "2.0.0", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(versions, "1.0.0"), 1));
    catalog_answer second = install("a", newer, "2.0.0", "linux-x64");
    CHECK(second.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(second.tree);
    char *directory = in_cache("bundles/a/1.0.0");
    expect_answer(check_directory(directory), "{\"kind\":\"intact\"}", "deep check");
    expect_answer(installed("linux-x64"), "{\"bundles\":{\"a\":[\"1.0.0\",\"2.0.0\"]},\"libraries\":{}}", "deep installed");
    expect_removed("[\"a@1.0.0\"]", "deep sweep");
    CHECK(!spisor_file_exists(directory));
    CHECK(bundle_present("a", "2.0.0"));
    free(directory);
    CHECK(runtime_remove_tree(deep) == 1);
    finish_cache();
}

/* cache.platform and the arguments */

static void it_answers_this_build_s_platform_key_as_node_spells_it(void) {
    catalog_answer answer = call_catalog(runtime_cache_platform, cJSON_CreateObject());
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    const char *platform = text_at(answer.tree, "platform");
#if defined(_WIN32) && defined(_M_X64)
    check_text(platform, "win32-x64", "platform");
#elif defined(__linux__) && defined(__x86_64__)
    check_text(platform, "linux-x64", "platform");
#elif defined(__APPLE__) && defined(__aarch64__)
    check_text(platform, "darwin-arm64", "platform");
#elif defined(__APPLE__) && defined(__x86_64__)
    check_text(platform, "darwin-x64", "platform");
#else
    CHECK(platform != NULL && strchr(platform, '-') != NULL);
#endif
    cJSON_Delete(answer.tree);
}

static void it_installs_for_this_machine_s_platform_when_none_is_given(void) {
    start_cache("default-platform");
    catalog_answer platform_answer = call_catalog(runtime_cache_platform, cJSON_CreateObject());
    const char *platform = text_at(platform_answer.tree, "platform");
    cJSON *entry = cJSON_CreateObject();
    cJSON *files = cJSON_AddObjectToObject(cJSON_AddObjectToObject(cJSON_AddObjectToObject(entry, "platforms"), platform), "files");
    cJSON_AddItemToObject(files, "liba.so", reference_to("liba.so", LIBRARY_SO));
    catalog_answer answer = install("liba", item_of("library", "2.0.0", entry), "2.0.0", NULL);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    char rest[256];
    snprintf(rest, sizeof rest, "libraries/liba/2.0.0/%s/liba.so", platform);
    CHECK(exists_in_cache(rest));
    cJSON_Delete(platform_answer.tree);
    finish_cache();
}

static void it_refuses_an_unusable_argument(void) {
    expect_refused(call_catalog(runtime_cache_install, parsed("{\"id\":\"a\",\"item\":{},\"version\":\"1.0.0\"}")), "cache",
                   "install without cache");
    expect_refused(call_catalog(runtime_cache_install, parsed("{\"cache\":\"/c\",\"item\":{},\"version\":\"1.0.0\"}")), "id",
                   "install without id");
    expect_refused(call_catalog(runtime_cache_install, parsed("{\"cache\":\"/c\",\"id\":\"a\",\"version\":\"1.0.0\"}")), "item",
                   "install without item");
    expect_refused(call_catalog(runtime_cache_install, parsed("{\"cache\":\"/c\",\"id\":\"a\",\"item\":{}}")), "version",
                   "install without version");
    expect_refused(call_catalog(runtime_cache_install,
                                parsed("{\"cache\":\"/c\",\"id\":\"a\",\"item\":{},\"version\":\"1.0.0\",\"platform\":1}")),
                   "platform", "install with a platform that is not text");
    expect_refused(call_catalog(runtime_cache_installed, parsed("{}")), "cache", "installed without cache");
    expect_refused(call_catalog(runtime_cache_check, parsed("{}")), "directory", "check without directory");
    expect_refused(call_catalog(runtime_cache_sweep, parsed("{\"cache\":\"/c\"}")), "state", "sweep without state");
    expect_refused(call_catalog(runtime_cache_sweep, parsed("{\"state\":\"/s\"}")), "cache", "sweep without cache");
}

int main(void) {
    it_fetches_into_a_fresh_store_then_answers_cached_without_fetching();
    it_stops_a_download_serving_more_than_the_size_the_catalog_lists();
    it_refetches_a_cached_file_whose_hash_no_longer_matches();
    it_refuses_a_source_serving_other_bytes_naming_the_url_and_the_expected_hash();
    it_refuses_a_fetched_file_whose_length_differs_from_the_size_even_when_the_hash_matches();
    it_lets_two_concurrent_stores_of_one_path_both_succeed();
#if defined(_WIN32)
    it_refuses_when_its_rename_fails_and_the_destination_holds_other_bytes();
#endif
    it_leaves_no_temporary_file_behind_even_when_the_rename_into_place_fails();

    it_installs_a_plugin_s_files_into_its_bundle_version_directory();
    it_marks_a_version_complete_last_naming_every_file_its_hash_and_its_size();
    it_reports_each_installed_version_a_start_would_refuse_or_whose_hash_changed();
    it_reports_nothing_for_a_version_whose_marker_is_gone();
    it_leaves_no_runnable_version_when_the_second_of_two_files_is_refused();
    it_stores_files_in_the_order_javascript_enumerates_their_keys();
    it_refuses_as_ambiguous_a_file_listed_both_for_every_platform_and_for_this_one();
    it_refuses_a_file_that_would_take_the_completion_marker_s_name();
    it_marks_a_version_of_no_files_complete_making_its_directory();
    it_answers_cached_for_every_file_on_a_second_install();
    it_installs_a_library_s_files_for_the_platform_into_its_library_directory();
    it_refuses_an_item_not_built_for_this_platform_with_the_fix();
    it_refuses_a_version_the_item_does_not_list();
    it_refuses_a_file_whose_destination_escapes_the_version_directory();
    it_refuses_an_id_or_a_platform_key_that_would_climb_out_of_the_cache();
    it_refuses_a_mismatched_file_naming_the_item_the_file_and_the_expected_hash();
    it_completes_an_interrupted_install_on_the_next_install();

    it_answers_intact_for_a_version_whose_files_carry_their_recorded_hashes();
    it_answers_absent_for_a_version_that_is_not_installed();
    it_answers_absent_for_a_version_with_no_completion_marker();
    it_answers_damaged_for_a_corrupted_file_naming_it();
    it_answers_damaged_for_a_missing_file();
    it_answers_damaged_for_a_completion_marker_that_is_not_readable();
    it_answers_damaged_for_a_marker_that_names_a_file_outside_its_version_directory();

    it_answers_each_id_s_versions_that_carry_a_readable_completion_marker();
    it_answers_nothing_for_a_cache_with_no_bundles_folder();
    it_leaves_out_a_version_whose_marker_names_a_file_that_is_missing();

    it_keeps_every_version_a_reference_file_names_and_the_newest_installed();
    it_sweeps_a_marked_version_whose_files_are_gone();
    it_keeps_the_versions_two_hosts_reference_files_name();
    it_keeps_a_version_a_host_scheduled_for_its_next_start();
    it_keeps_the_newest_installed_version_of_an_id_no_host_references();
    it_leaves_a_version_with_no_completion_marker();
    it_does_not_follow_a_reference_file_s_names_out_of_the_bundles_folder();
    it_answers_nothing_removed_for_a_cache_with_no_bundles();
    it_sweeps_library_versions_per_platform_like_bundles();
    it_leaves_a_library_version_with_no_completion_marker_alone();
    it_removes_an_unmarked_bundle_version_whose_newest_file_is_more_than_a_day_old();
    it_keeps_an_unmarked_bundle_version_while_any_one_of_its_files_is_recent();
    it_keeps_an_old_unmarked_bundle_version_a_host_references();
    it_keys_an_abandoned_id_folder_by_its_id_when_it_checks_references_and_names_what_it_removes();
    it_removes_an_old_unmarked_library_version_for_one_platform();
    it_skips_a_reference_file_that_is_not_two_maps_of_versions();
    it_keeps_what_a_reference_file_keeps_for_rollback();
    it_keeps_what_a_boot_hint_names_for_the_next_start();
    it_prunes_a_run_record_whose_process_is_gone_and_its_versions_with_it();
    it_prunes_a_run_record_whose_process_stopped_stamping();
    it_keeps_what_a_live_start_s_run_record_names();
    it_sweeps_only_while_it_holds_the_registry_lock();

    it_stores_checks_and_sweeps_under_a_path_over_260_characters();

    it_answers_this_build_s_platform_key_as_node_spells_it();
    it_installs_for_this_machine_s_platform_when_none_is_given();
    it_refuses_an_unusable_argument();
    return check_summary("cache_test");
}
