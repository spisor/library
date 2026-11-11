#include "updates_fixture.h"

#include "cache_place.h"
#include "hosts_lock.h"
#include "platform_key.h"
#include "spisor/json_write.h"

#if !defined(_WIN32)
#include <time.h>
#endif

#define HOST "spisor-host"
#define LIBRARIES "{\"runtime\":\"0.1.0\",\"koffi\":\"2.0.0\"}"
#define DECLARATION "library.json"

static catalog_answer read_hint(const char *host) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", host);
    return call_catalog(runtime_boot_read, argument);
}

static catalog_answer write_hint_on(const char *host, const char *libraries_json, const char *version,
                                    const char *platform) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", host);
    cJSON_AddItemToObject(argument, "libraries", parsed(libraries_json));
    if (version != NULL) {
        cJSON_AddStringToObject(argument, "host", version);
    }
    if (platform != NULL) {
        cJSON_AddStringToObject(argument, "platform", platform);
    }
    return call_catalog(runtime_boot_write, argument);
}

static catalog_answer write_hint_through_boot(const char *host, const char *libraries_json, const char *version) {
    return write_hint_on(host, libraries_json, version, NULL);
}

static char *library_folder(const char *id, const char *version, const char *platform) {
    const char *segments[4] = {"libraries", id, version, platform};
    return runtime_cache_resolve(cache, segments, 4);
}

static void place_library(const char *id, const char *version, const char *platform, const char *const *files,
                          size_t count) {
    char *folder = library_folder(id, version, platform);
    for (size_t index = 0; index < count; index++) {
        write_in(folder, files[index], files[index]);
    }
    free(folder);
}

/** The entry a hint records for id at version on platform: its folder, and binary_name inside it unless NULL. */
static cJSON *hinted_entry(const char *id, const char *version, const char *platform, const char *binary_name) {
    char *folder = library_folder(id, version, platform);
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "version", version);
    cJSON_AddStringToObject(entry, "directory", folder);
    if (binary_name != NULL) {
        const char *segments[2] = {folder, binary_name};
        char *binary = runtime_cache_resolve(cache, segments, 2);
        cJSON_AddStringToObject(entry, "binary", binary);
        free(binary);
    }
    free(folder);
    return entry;
}

/** The hint a write of versions_json on this machine's platform records, with no library declared, as JSON text. */
static char *undeclared_hint(const char *versions_json, const char *host) {
    cJSON *versions = parsed(versions_json);
    cJSON *hint = cJSON_CreateObject();
    cJSON *libraries = cJSON_AddObjectToObject(hint, "libraries");
    for (const cJSON *entry = versions->child; entry != NULL; entry = entry->next) {
        cJSON_AddItemToObject(libraries, entry->string,
                              hinted_entry(entry->string, entry->valuestring, runtime_platform_key(), NULL));
    }
    if (host != NULL) {
        cJSON_AddStringToObject(hint, "host", host);
    }
    char *text = cJSON_PrintUnformatted(hint);
    cJSON_Delete(hint);
    cJSON_Delete(versions);
    return text;
}

static void expect_entry(const cJSON *tree, const char *id, const cJSON *expected, const char *label) {
    char *text = cJSON_PrintUnformatted(expected);
    expect_json(value_at(tree, id), text, label);
    cJSON_free(text);
}

static void install_host(const char *version) {
    list_host(HOST, version, version);
    install_listed(HOST, version);
}

static void it_writes_the_hint_as_json_stringify_writes_it_and_reads_it_back(void) {
    start_base("hint-round-trip");
    catalog_answer written = write_hint_through_boot(HOST, LIBRARIES, "1.0.0");
    expect_success(written, "round trip write");
    cJSON_Delete(written.tree);
    char *expected = undeclared_hint(LIBRARIES, "1.0.0");
    cJSON *expected_tree = parsed(expected);
    char *pretty = spisor_json_stringify_pretty(expected_tree);
    char *text = malloc(strlen(pretty) + 2);
    snprintf(text, strlen(pretty) + 2, "%s\n", pretty);
    expect_text_in(cache, "hosts/spisor-host.json", text, "round trip text");
    catalog_answer read = read_hint(HOST);
    expect_member_json(read.tree, "hint", expected, "round trip read");
    cJSON_Delete(read.tree);
    free(text);
    free(pretty);
    cJSON_Delete(expected_tree);
    cJSON_free(expected);
    finish_base();
}

static void it_keeps_the_recorded_host_version_when_a_write_names_none(void) {
    start_base("hint-keeps-host");
    cJSON_Delete(write_hint_through_boot(HOST, LIBRARIES, "1.0.0").tree);
    cJSON_Delete(write_hint_through_boot(HOST, "{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"}", NULL).tree);
    char *expected = undeclared_hint("{\"runtime\":\"0.2.0\",\"koffi\":\"2.0.0\"}", "1.0.0");
    expect_hint(HOST, expected, "keeps host");
    cJSON_free(expected);
    finish_base();
}

static void it_records_the_binary_of_each_hinted_library(void) {
    start_base("hint-binaries");
    const char *runtime_files[2] = {DECLARATION, "libruntime.so"};
    const char *forebay_files[2] = {DECLARATION, "libforebay.so"};
    place_library("runtime", "0.1.0", "linux-x64", runtime_files, 2);
    place_library("forebay-library", "0.1.2", "linux-x64", forebay_files, 2);
    catalog_answer written =
        write_hint_on(HOST, "{\"runtime\":\"0.1.0\",\"forebay-library\":\"0.1.2\"}", "1.0.0", "linux-x64");
    expect_success(written, "binaries write");
    cJSON *runtime = hinted_entry("runtime", "0.1.0", "linux-x64", "libruntime.so");
    cJSON *forebay = hinted_entry("forebay-library", "0.1.2", "linux-x64", "libforebay.so");
    expect_entry(written.tree, "hint/libraries/runtime", runtime, "runtime binary");
    expect_entry(written.tree, "hint/libraries/forebay-library", forebay, "forebay binary");
    cJSON_Delete(forebay);
    cJSON_Delete(runtime);
    cJSON_Delete(written.tree);
    finish_base();
}

static void expect_binary_on(const char *platform, const char *binary_name) {
    const char *files[4] = {DECLARATION, "runtime.dll", "libruntime.dylib", "libruntime.so"};
    place_library("runtime", "0.1.0", platform, files, 4);
    catalog_answer written = write_hint_on(HOST, "{\"runtime\":\"0.1.0\"}", NULL, platform);
    expect_success(written, platform);
    cJSON *expected = hinted_entry("runtime", "0.1.0", platform, binary_name);
    expect_entry(written.tree, "hint/libraries/runtime", expected, platform);
    cJSON_Delete(expected);
    cJSON_Delete(written.tree);
}

static void it_names_the_binary_by_the_platform_suffix(void) {
    start_base("hint-suffix");
    expect_binary_on("win32-x64", "runtime.dll");
    expect_binary_on("darwin-arm64", "libruntime.dylib");
    expect_binary_on("linux-x64", "libruntime.so");
    finish_base();
}

static void expect_refused_folder(const char *const *files, size_t count, const char *label) {
    place_library("runtime", "0.1.0", "linux-x64", files, count);
    catalog_answer written = write_hint_on(HOST, "{\"runtime\":\"0.1.0\"}", "1.0.0", "linux-x64");
    char *folder = library_folder("runtime", "0.1.0", "linux-x64");
    expect_refusal(written, folder, label);
    expect_fix(written, REINSTALL_FIX, label);
    CHECK(!exists_in(cache, "hosts/spisor-host.json"));
    cJSON_Delete(written.tree);
    free(folder);
}

static void it_refuses_a_library_folder_with_no_binary(void) {
    start_base("hint-no-binary");
    const char *files[3] = {DECLARATION, "runtime.dll", "notes.txt"};
    expect_refused_folder(files, 3, "no binary");
    finish_base();
}

static void it_refuses_a_library_folder_with_two_binaries(void) {
    start_base("hint-two-binaries");
    const char *files[3] = {DECLARATION, "libruntime.so", "libhelper.so"};
    expect_refused_folder(files, 3, "two binaries");
    finish_base();
}

static void it_records_only_the_directory_of_a_library_with_no_declaration(void) {
    start_base("hint-undeclared");
    const char *files[2] = {"lib/native.node", "package.json"};
    place_library("koffi", "2.16.3", "linux-x64", files, 2);
    catalog_answer written = write_hint_on(HOST, "{\"koffi\":\"2.16.3\"}", "1.0.0", "linux-x64");
    expect_success(written, "undeclared write");
    cJSON *expected = hinted_entry("koffi", "2.16.3", "linux-x64", NULL);
    expect_entry(written.tree, "hint/libraries/koffi", expected, "directory only");
    cJSON_Delete(expected);
    cJSON_Delete(written.tree);
    finish_base();
}

static void it_reads_back_what_it_wrote(void) {
    start_base("hint-read-back");
    const char *runtime_files[2] = {DECLARATION, "runtime.dll"};
    const char *koffi_files[1] = {"lib/native.node"};
    place_library("runtime", "0.1.0", "win32-x64", runtime_files, 2);
    place_library("koffi", "2.16.3", "win32-x64", koffi_files, 1);
    catalog_answer written = write_hint_on(HOST, "{\"runtime\":\"0.1.0\",\"koffi\":\"2.16.3\"}", "1.0.0", "win32-x64");
    if (!expect_success(written, "read back write")) {
        cJSON_Delete(written.tree);
        finish_base();
        return;
    }
    catalog_answer read = read_hint(HOST);
    char *expected = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(written.tree, "hint"));
    CHECK(value_at(written.tree, "hint/libraries/runtime/binary") != NULL);
    expect_member_json(read.tree, "hint", expected, "read back");
    expect_hint(HOST, expected, "read back file");
    cJSON_free(expected);
    cJSON_Delete(read.tree);
    cJSON_Delete(written.tree);
    finish_base();
}

static void it_refuses_a_platform_that_is_no_platform_key(void) {
    start_base("hint-bad-platform");
    catalog_answer written = write_hint_on(HOST, LIBRARIES, "1.0.0", "Linux_x64");
    expect_refusal(written, "platform", "bad platform");
    cJSON_Delete(written.tree);
    finish_base();
}

static void it_reads_no_hint_when_there_is_none_or_it_is_not_readable_json(void) {
    start_base("hint-none");
    catalog_answer none = read_hint(HOST);
    expect_success(none, "no hint");
    CHECK(none.tree != NULL && cJSON_GetObjectItemCaseSensitive(none.tree, "hint") == NULL);
    cJSON_Delete(none.tree);
    write_hint(HOST, "{ not json");
    catalog_answer corrupt = read_hint(HOST);
    CHECK(corrupt.tree != NULL && cJSON_GetObjectItemCaseSensitive(corrupt.tree, "hint") == NULL);
    cJSON_Delete(corrupt.tree);
    write_hint(HOST, "[1]");
    catalog_answer listed = read_hint(HOST);
    CHECK(listed.tree != NULL && cJSON_GetObjectItemCaseSensitive(listed.tree, "hint") == NULL);
    cJSON_Delete(listed.tree);
    finish_base();
}

static void expect_no_hint_in(const char *recorded, const char *label) {
    write_hint(HOST, recorded);
    catalog_answer read = read_hint(HOST);
    expect_success(read, label);
    if (read.tree == NULL || cJSON_GetObjectItemCaseSensitive(read.tree, "hint") != NULL) {
        fprintf(stderr, "%s: expected no hint\n", label);
        check_failures++;
    }
    cJSON_Delete(read.tree);
}

static void it_reads_a_hint_whose_library_entry_is_not_a_version_and_directory_as_no_hint(void) {
    start_base("hint-entry-shape");
    expect_no_hint_in("{\"libraries\":{\"runtime\":\"0.1.0\"},\"host\":\"1.0.0\"}", "a version string");
    expect_no_hint_in("{\"libraries\":{\"runtime\":{\"version\":\"0.1.0\"}}}", "no directory");
    expect_no_hint_in("{\"libraries\":{\"runtime\":{\"version\":\"0.1.0\",\"directory\":\"d\",\"binary\":2}}}",
                      "a binary that is no text");
    expect_no_hint_in("{\"libraries\":[]}", "libraries no object");
    finish_base();
}

static void it_reads_a_host_that_is_no_text_as_none(void) {
    start_base("hint-host-shape");
    write_hint(HOST, "{\"libraries\":{\"runtime\":{\"version\":\"0.1.0\",\"directory\":\"d\"}},\"host\":3}");
    catalog_answer read = read_hint(HOST);
    expect_member_json(read.tree, "hint", "{\"libraries\":{\"runtime\":{\"version\":\"0.1.0\",\"directory\":\"d\"}}}",
                       "host dropped");
    cJSON_Delete(read.tree);
    finish_base();
}

static void it_refuses_a_host_id_that_would_record_its_libraries_outside_the_hosts_folder(void) {
    start_base("hint-escape");
    expect_refusal(write_hint_through_boot("../escape", LIBRARIES, "1.0.0"), "outside", "escape write");
    expect_refusal(read_hint("../escape"), "outside", "escape read");
    expect_refusal(read_hint(""), NULL, "empty id");
    CHECK(!exists_in(scratch, "escape.json"));
    finish_base();
}

static void it_resolves_the_executable_of_the_version_the_hint_names_with_the_arguments(void) {
    start_base("resolve-hinted");
    install_host("1.0.0");
    install_host("2.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    catalog_answer resolved = resolve_boot(HOST, "[\"launch\",\"cc\"]");
    expect_success(resolved, "hinted");
    expect_executable(resolved, HOST, "1.0.0", "hinted executable");
    expect_member_json(resolved.tree, "arguments", "[\"launch\",\"cc\"]", "hinted arguments");
    check_text(text_at(resolved.tree, "version"), "1.0.0", "hinted version");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_names_the_windows_executable_with_its_extension(void) {
    start_base("resolve-windows");
    list_version(HOST, "host", "1.0.0", "1.0.0", "win32-x64/spisor-host.exe", NULL);
    install_listed(HOST, "1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", HOST);
    cJSON_AddStringToObject(argument, "platform", "win32-x64");
    catalog_answer resolved = call_catalog(runtime_boot_resolve, argument);
    expect_success(resolved, "windows");
    const char *executable = text_at(resolved.tree, "executable");
    CHECK(executable != NULL && strstr(executable, "spisor-host.exe") != NULL && spisor_file_exists(executable));
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_when_no_host_id_is_given(void) {
    start_base("resolve-no-id");
    catalog_answer resolved = resolve_boot(NULL, NULL);
    expect_refusal(resolved, "host id", "no id");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_with_the_reinstall_fix_when_there_is_no_hint(void) {
    start_base("resolve-no-hint");
    install_host("1.0.0");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "no host version is recorded for spisor-host", "no hint");
    expect_fix(resolved, REINSTALL_FIX, "no hint fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_with_the_reinstall_fix_when_the_hint_is_corrupt_never_picking_an_installed_version(void) {
    start_base("resolve-corrupt");
    install_host("1.0.0");
    write_hint(HOST, "{ not json");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "no host version is recorded", "corrupt");
    expect_fix(resolved, REINSTALL_FIX, "corrupt fix");
    CHECK(text_at(resolved.tree, "executable") == NULL);
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_a_hint_that_names_a_version_outside_the_bundles_folder(void) {
    start_base("resolve-outside");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"../../escape\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "is not a version", "outside");
    expect_fix(resolved, REINSTALL_FIX, "outside fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_a_host_bundle_whose_file_no_longer_has_its_recorded_size(void) {
    start_base("resolve-resized");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    write_in(cache, "bundles/spisor-host/1.0.0/" PLATFORM "/spisor-host", "resized");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "was not started", "resized");
    expect_refusal(resolved, "has 7 bytes, but its install recorded 17", "resized size");
    expect_fix(resolved, REINSTALL_FIX, "resized fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

/** A start checks sizes alone; the same-size corruption is status's to find, which hashes every file. */
static void it_starts_a_host_bundle_whose_file_kept_its_size_without_hashing_it(void) {
    start_base("resolve-same-size");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    write_in(cache, "bundles/spisor-host/1.0.0/" PLATFORM "/spisor-host", "spisor-host@9.9.9");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_success(resolved, "same size");
    expect_executable(resolved, HOST, "1.0.0", "same size executable");
    cJSON_Delete(resolved.tree);
    finish_base();
}

/** A completion marker from before sizes were recorded is no proof of a version a start may run. */
static void it_refuses_a_host_bundle_whose_marker_records_no_sizes(void) {
    start_base("resolve-no-sizes");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    char *recorded = text_in(cache, "bundles/spisor-host/1.0.0/.installed.json");
    cJSON *marker = recorded == NULL ? NULL : spisor_parse_json((const uint8_t *)recorded, strlen(recorded));
    free(recorded);
    CHECK(cJSON_HasObjectItem(marker, "sizes"));
    cJSON_DeleteItemFromObjectCaseSensitive(marker, "sizes");
    char *text = cJSON_PrintUnformatted(marker);
    write_in(cache, "bundles/spisor-host/1.0.0/.installed.json", text);
    cJSON_free(text);
    cJSON_Delete(marker);
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "records no size for", "no sizes");
    expect_fix(resolved, REINSTALL_FIX, "no sizes fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_when_the_version_the_hint_names_is_not_installed(void) {
    start_base("resolve-absent");
    install_host("1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"9.9.9\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "is not installed", "absent");
    expect_fix(resolved, REINSTALL_FIX, "absent fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_refuses_an_installed_version_that_carries_no_executable_for_the_platform(void) {
    start_base("resolve-no-executable");
    mark_installed("bundles", HOST, "1.0.0");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_refusal(resolved, "carries no executable", "no executable");
    expect_fix(resolved, REINSTALL_FIX, "no executable fix");
    cJSON_Delete(resolved.tree);
    finish_base();
}

static void it_leaves_everything_the_hint_names_after_a_sweep(void) {
    start_base("sweep-keeps-hint");
    install_host("1.0.0");
    install_host("2.0.0");
    install_host("3.0.0");
    mark_installed("libraries", "runtime", "0.1.0");
    mark_installed("libraries", "runtime", "0.2.0");
    mark_installed("libraries", "koffi", "2.0.0");
    write_hint(HOST, "{\"libraries\":{\"runtime\":{\"version\":\"0.1.0\",\"directory\":\"r\"},"
                     "\"koffi\":{\"version\":\"2.0.0\",\"directory\":\"k\"}},\"host\":\"2.0.0\"}");
    record_references(HOST, "{\"run\":{},\"next\":{}}");
    catalog_answer swept = sweep_cache();
    expect_success(swept, "sweep");
    cJSON_Delete(swept.tree);
    CHECK(exists_in(cache, "bundles/spisor-host/2.0.0/" PLATFORM "/spisor-host"));
    CHECK(!exists_in(cache, "bundles/spisor-host/1.0.0"));
    CHECK(exists_in(cache, "libraries/runtime/0.1.0/" PLATFORM));
    CHECK(exists_in(cache, "libraries/koffi/2.0.0/" PLATFORM));
    catalog_answer resolved = resolve_boot(HOST, NULL);
    expect_executable(resolved, HOST, "2.0.0", "after sweep");
    cJSON_Delete(resolved.tree);
    finish_base();
}

typedef struct locked_write {
    catalog_answer answer;
} locked_write;

static void write_with_state(locked_write *job) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", cache);
    cJSON_AddStringToObject(argument, "hostId", HOST);
    cJSON_AddItemToObject(argument, "libraries", parsed(LIBRARIES));
    cJSON_AddStringToObject(argument, "state", state);
    job->answer = call_catalog(runtime_boot_write, argument);
}

#if defined(_WIN32)
static DWORD WINAPI writing_entry(LPVOID argument) {
    write_with_state(argument);
    return 0;
}
#else
static void *writing_entry(void *argument) {
    write_with_state(argument);
    return NULL;
}
#endif

static void wait_a_moment(void) {
#if defined(_WIN32)
    Sleep(400);
#else
    struct timespec pause = {0, 400000000L};
    nanosleep(&pause, NULL);
#endif
}

static void it_writes_a_hint_with_a_state_only_once_the_registry_lock_is_free(void) {
    start_base("hint-locked");
    write_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}");
    spisor_lock *guard = runtime_hosts_lock(state);
    CHECK(guard != NULL);
    locked_write job = {{0, NULL}};
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, writing_entry, &job, 0, NULL);
    CHECK(thread != NULL);
#else
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, writing_entry, &job) == 0);
#endif
    wait_a_moment();
    expect_hint(HOST, "{\"libraries\":{},\"host\":\"1.0.0\"}", "still waiting for the lock");
    spisor_unlock(guard);
#if defined(_WIN32)
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    expect_success(job.answer, "locked write");
    cJSON_Delete(job.answer.tree);
    char *expected = undeclared_hint(LIBRARIES, "1.0.0");
    expect_hint(HOST, expected, "written once the lock was free");
    cJSON_free(expected);
    finish_base();
}

int main(void) {
    it_writes_the_hint_as_json_stringify_writes_it_and_reads_it_back();
    it_keeps_the_recorded_host_version_when_a_write_names_none();
    it_reads_no_hint_when_there_is_none_or_it_is_not_readable_json();
    it_reads_a_hint_whose_library_entry_is_not_a_version_and_directory_as_no_hint();
    it_reads_a_host_that_is_no_text_as_none();
    it_records_the_binary_of_each_hinted_library();
    it_names_the_binary_by_the_platform_suffix();
    it_refuses_a_library_folder_with_no_binary();
    it_refuses_a_library_folder_with_two_binaries();
    it_records_only_the_directory_of_a_library_with_no_declaration();
    it_reads_back_what_it_wrote();
    it_refuses_a_platform_that_is_no_platform_key();
    it_refuses_a_host_id_that_would_record_its_libraries_outside_the_hosts_folder();

    it_resolves_the_executable_of_the_version_the_hint_names_with_the_arguments();
    it_names_the_windows_executable_with_its_extension();
    it_refuses_when_no_host_id_is_given();
    it_refuses_with_the_reinstall_fix_when_there_is_no_hint();
    it_refuses_with_the_reinstall_fix_when_the_hint_is_corrupt_never_picking_an_installed_version();
    it_refuses_a_hint_that_names_a_version_outside_the_bundles_folder();
    it_refuses_a_host_bundle_whose_file_no_longer_has_its_recorded_size();
    it_starts_a_host_bundle_whose_file_kept_its_size_without_hashing_it();
    it_refuses_a_host_bundle_whose_marker_records_no_sizes();
    it_refuses_when_the_version_the_hint_names_is_not_installed();
    it_refuses_an_installed_version_that_carries_no_executable_for_the_platform();

    it_leaves_everything_the_hint_names_after_a_sweep();
    it_writes_a_hint_with_a_state_only_once_the_registry_lock_is_free();
    return check_summary("boot_hint_test");
}
