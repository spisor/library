#include "install_fixture.h"
#include "spisor/allocation.h"
#include "install_call.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#endif

#include "install_hooks.h"
#include "file_times.h"
#include "install_runtime.h"
#include "process_run.h"

#define TEST_VERSION "1.2.3"
#define MINIMUM "22.18.0"
#if defined(_WIN32)
#define ARCHIVE_ENDING ".zip"
#define EXECUTABLE_MEMBER "node.exe"
#else
#define ARCHIVE_ENDING ".tar.gz"
#define EXECUTABLE_MEMBER "bin/node"
#endif

/** The folder TEST_VERSION's archive unpacks into, and that archive's name, both as Node names them. */
static char STEM[64];
static char ARCHIVE_NAME[80];

/** rest inside STEM, in a buffer the next call reuses. */
static const char *in_stem(const char *rest) {
    static char path[256];
    snprintf(path, sizeof path, "%s/%s", STEM, rest);
    return path;
}

typedef struct runtime_fixture {
    install_base base;
    char *archive;
    char *pinned_url;
    char archive_sha256[NAULA_SHA256_HEX_SIZE];
    const char *pinned_sha256;
} runtime_fixture;

static char *system_tar(void) {
#if defined(_WIN32)
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    char narrow[4 * MAX_PATH];
    int written = WideCharToMultiByte(CP_UTF8, 0, folder, (int)length, narrow, (int)sizeof narrow - 1, NULL, NULL);
    narrow[written] = 0;
    return joined(narrow, "tar.exe");
#else
    const char *tar = spisor_probe_file("/usr/bin/tar") == SPISOR_FILE_PRESENT ? "/usr/bin/tar" : "/bin/tar";
    char *copy = spisor_memory_allocate(strlen(tar) + 1);
    strcpy(copy, tar);
    return copy;
#endif
}

/**
 * Packs an archive served as served_name holding packed_stem's executable beside a README and a lib folder, as
 * Node's archives do.
 */
static void pack_archive_as(runtime_fixture *fixture, const char *executable_content, const char *packed_stem,
                            const char *served_name) {
    char *packing = joined(fixture->base.root, "packing");
    char *stem = joined(packing, packed_stem);
    write_in(stem, EXECUTABLE_MEMBER, executable_content);
    write_in(stem, "README.md", "readme");
    write_in(stem, "lib/node_modules/npm/package.json", "{}");
    char *served_folder = joined(fixture->base.root, "served");
    fixture->archive = joined(served_folder, served_name);
    spisor_memory_release(served_folder);
    char *served = joined(fixture->base.root, "served");
    CHECK(spisor_make_directories(served) == SPISOR_OK);
    cJSON *arguments = cJSON_CreateArray();
#if defined(_WIN32)
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-a"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-cf"));
#else
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-czf"));
#endif
    cJSON_AddItemToArray(arguments, cJSON_CreateString(fixture->archive));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-C"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(packing));
    cJSON_AddItemToArray(arguments, cJSON_CreateString(packed_stem));
    char *tar = system_tar();
    int exit_code = -1;
    char *problem = NULL;
    CHECK(runtime_run_to_exit(tar, arguments, 120000, &exit_code, NULL, &problem) && exit_code == 0);
    CHECK(spisor_sha256_file(fixture->archive, fixture->archive_sha256) == SPISOR_OK);
    spisor_memory_release(problem);
    spisor_memory_release(tar);
    cJSON_Delete(arguments);
    spisor_memory_release(served);
    spisor_memory_release(stem);
    spisor_memory_release(packing);
}

/** typescript-host 1.0.0 installed and hinted under cache, its host.json pinning Node pinned at url with sha256. */
static void install_pin_of(const char *cache, const char *pinned, const char *url, const char *sha256) {
    write_in(cache, "hosts/typescript-host.json", "{\"libraries\":{},\"host\":\"1.0.0\"}");
    char *manifest = typescript_host_json(pinned, MINIMUM, url, sha256);
    write_in(cache, "bundles/typescript-host/1.0.0/host.json", manifest);
    spisor_memory_release(manifest);
}

static void install_node_pin(const char *cache, const char *url, const char *sha256) {
    install_pin_of(cache, TEST_VERSION, url, sha256);
}

/** A base serving TEST_VERSION's archive as served_name, pinned as the build for this platform. */
static void start_runtime_serving(runtime_fixture *fixture, const char *label, const char *served_name) {
    start_install_base(&fixture->base, "runtime", label);
    pack_archive_as(fixture, "the node executable", STEM, served_name);
    fixture->pinned_url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, fixture->archive, &fixture->pinned_url);
    fixture->pinned_sha256 = fixture->archive_sha256;
    install_node_pin(fixture->base.cache, fixture->pinned_url, fixture->pinned_sha256);
}

static void start_runtime(runtime_fixture *fixture, const char *label, const char *sha256_override) {
    start_runtime_serving(fixture, label, ARCHIVE_NAME);
    if (sha256_override != NULL) {
        fixture->pinned_sha256 = sha256_override;
        install_node_pin(fixture->base.cache, fixture->pinned_url, fixture->pinned_sha256);
    }
}

static void finish_runtime(runtime_fixture *fixture) {
    spisor_memory_release(fixture->pinned_url);
    spisor_memory_release(fixture->archive);
    finish_install_base(&fixture->base);
}

/** install.runtime for base's cache, looking for a node on path. */
static catalog_answer acquire_on(const install_base *base, const char *path) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", base->cache);
    cJSON_AddItemToObject(argument, "environment", environment_with_path(base, path));
    catalog_answer answer = install_call(INSTALL_RUNTIME, argument);
    if (answer.status != SPISOR_OK) {
        char *printed = answer.tree == NULL ? NULL : cJSON_PrintUnformatted(answer.tree);
        fprintf(stderr, "install.runtime answered %d: %s\n", (int)answer.status, printed == NULL ? "(nothing)" : printed);
        cJSON_free(printed);
    }
    return answer;
}

static catalog_answer acquire(const runtime_fixture *fixture) {
    return acquire_on(&fixture->base, fixture->base.path);
}

static char *runtime_directory(const runtime_fixture *fixture) {
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/" TEST_VERSION "/%s", runtime_platform_key());
    return joined(fixture->base.cache, rest);
}

static char *executable_path(const runtime_fixture *fixture) {
    char *directory = runtime_directory(fixture);
    char *path = joined(directory, in_stem(EXECUTABLE_MEMBER));
    spisor_memory_release(directory);
    return path;
}

static void acquires_the_pinned_runtime_and_answers_its_executable(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "acquires", NULL);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK);
    char *executable = executable_path(&fixture);
    char expected[2048];
    cJSON *quoted = cJSON_CreateString(executable);
    char *executable_json = cJSON_PrintUnformatted(quoted);
    snprintf(expected, sizeof expected, "{\"name\":\"node\",\"version\":\"" TEST_VERSION "\",\"executable\":%s,\"fetched\":true,\"source\":\"acquired\"}",
             executable_json);
    expect_json(answer.tree, expected, "acquired");
    CHECK(spisor_probe_file(executable) == SPISOR_FILE_PRESENT);
    cJSON_free(executable_json);
    cJSON_Delete(quoted);
    cJSON_Delete(answer.tree);
    spisor_memory_release(executable);
    finish_runtime(&fixture);
}

static void fetches_nothing_on_a_second_run(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "second", NULL);
    cJSON_Delete(acquire(&fixture).tree);
    CHECK(runtime_remove_file_entry(fixture.archive) == RUNTIME_ENTRY_DONE);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    cJSON_Delete(answer.tree);
    finish_runtime(&fixture);
}

static void keeps_only_the_executable_and_the_archive_s_hash_in_place_of_the_archive(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "only-executable", NULL);
    cJSON_Delete(acquire(&fixture).tree);
    char *directory = runtime_directory(&fixture);
    char *executable = executable_path(&fixture);
    CHECK(!exists_in(directory, ARCHIVE_NAME));
    CHECK(!exists_in(directory, in_stem("README.md")) && !exists_in(directory, in_stem("lib")));
    char executable_sha256[NAULA_SHA256_HEX_SIZE];
    CHECK(spisor_sha256_file(executable, executable_sha256) == SPISOR_OK);
    char expected[200];
    snprintf(expected, sizeof expected, "%s\n%s\n", fixture.archive_sha256, executable_sha256);
    char *marker = text_in(directory, ".unpacked");
    check_text(marker, expected, "unpacked marker");
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == SPISOR_OK);
    size_t visible = 0;
    for (size_t index = 0; index < listing.count; index++) {
        if (listing.entries[index].name[0] != '.') {
            visible++;
            CHECK(strcmp(listing.entries[index].name, STEM) == 0);
        }
    }
    CHECK(visible == 1);
    runtime_free_directory_listing(&listing);
    spisor_memory_release(marker);
    spisor_memory_release(executable);
    spisor_memory_release(directory);
    finish_runtime(&fixture);
}

static void acquires_the_runtime_again_when_its_executable_s_bytes_changed(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "changed", NULL);
    cJSON_Delete(acquire(&fixture).tree);
    char *executable = executable_path(&fixture);
    write_file(executable, "altered bytes");
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    size_t length = 0;
    uint8_t *bytes = spisor_read_file_reporting(executable, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    CHECK(bytes != NULL && length == strlen("the node executable") && memcmp(bytes, "the node executable", length) == 0);
    spisor_memory_release(bytes);
    cJSON_Delete(answer.tree);
    spisor_memory_release(executable);
    finish_runtime(&fixture);
}

static void removes_the_runtime_versions_no_longer_pinned(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "stale", NULL);
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/1.0.0/%s/node", runtime_platform_key());
    write_in(fixture.base.cache, rest, "an older pin");
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK);
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(fixture.base.cache, "runtimes/node/1.0.0"));
    char *executable = executable_path(&fixture);
    CHECK(spisor_probe_file(executable) == SPISOR_FILE_PRESENT);
    spisor_memory_release(executable);
    finish_runtime(&fixture);
}

static void refuses_an_archive_whose_hash_differs_from_the_pin_and_leaves_no_executable(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "mismatch", "0000000000000000000000000000000000000000000000000000000000000000");
    catalog_answer answer = acquire(&fixture);
    expect_cache_refusal_refused(answer, NULL, "hash mismatch");
    cJSON_Delete(answer.tree);
    char *directory = runtime_directory(&fixture);
    char *executable = executable_path(&fixture);
    CHECK(spisor_probe_file(executable) != SPISOR_FILE_PRESENT && !exists_in(directory, ".unpacked") &&
        !exists_in(directory, ARCHIVE_NAME));
    spisor_memory_release(executable);
    spisor_memory_release(directory);
    finish_runtime(&fixture);
}

static void refuses_a_platform_with_no_pinned_build(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "unpinned", NULL);
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", fixture.base.cache);
    cJSON_AddItemToObject(argument, "environment", environment_of(&fixture.base));
    cJSON_AddStringToObject(argument, "platform", "aix-ppc64");
    catalog_answer answer = install_call(INSTALL_RUNTIME, argument);
    expect_cache_refusal_refused(answer, "aix-ppc64", "no pinned build");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(fixture.base.cache, "runtimes"));
    finish_runtime(&fixture);
}

/** Whether directory holds exactly one entry, named name, printing what it holds when not. */
static int holds_only(const char *directory, const char *name) {
    runtime_directory_listing listing;
    if (runtime_list_directory(directory, &listing) != SPISOR_OK) {
        fprintf(stderr, "%s cannot be listed\n", directory);
        return 0;
    }
    int only = listing.count == 1 && strcmp(listing.entries[0].name, name) == 0;
    for (size_t index = 0; !only && index < listing.count; index++) {
        fprintf(stderr, "%s holds %s, where only %s belongs\n", directory, listing.entries[index].name, name);
    }
    runtime_free_directory_listing(&listing);
    return only;
}

static void names_the_archive_and_its_folder_from_the_pin_never_from_its_url(void) {
    runtime_fixture fixture;
    start_runtime_serving(&fixture, "hostile-url", ".." ARCHIVE_ENDING);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK);
    char *executable = executable_path(&fixture);
    check_text(text_at(answer.tree, "executable"), executable, "the executable in the pin's own folder");
    char *versions = joined(fixture.base.cache, "runtimes/node");
    char *version = joined(versions, TEST_VERSION);
    CHECK(holds_only(versions, TEST_VERSION) && holds_only(version, runtime_platform_key()));
    spisor_memory_release(version);
    spisor_memory_release(versions);
    spisor_memory_release(executable);
    cJSON_Delete(answer.tree);
    finish_runtime(&fixture);
}

/**
 * A pinned version that is no plain x.y.z, served with an archive that unpacks under packed_version, beside an older
 * runtime already in the cache: refused, nothing written and nothing removed.
 */
static void refuses_a_pinned_version_that_is_not_plain(const char *label, const char *pinned,
                                                       const char *packed_version) {
    runtime_fixture fixture;
    start_install_base(&fixture.base, "runtime", label);
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/0.9.0/%s/node", runtime_platform_key());
    write_in(fixture.base.cache, rest, "an older pin");
    char packed[64];
    char served[96];
    node_archive_stem(packed_version, packed, sizeof packed);
    snprintf(served, sizeof served, "served" ARCHIVE_ENDING);
    pack_archive_as(&fixture, "the node executable", packed, served);
    fixture.pinned_url = NULL;
    runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, fixture.archive, &fixture.pinned_url);
    install_pin_of(fixture.base.cache, pinned, fixture.pinned_url, fixture.archive_sha256);
    catalog_answer answer = acquire(&fixture);
    expect_cache_refusal_refused(answer, "is not {pinned, minimum, builds}", label);
    cJSON_Delete(answer.tree);
    char *runtimes = joined(fixture.base.cache, "runtimes");
    char *versions = joined(runtimes, "node");
    CHECK(holds_only(runtimes, "node") && holds_only(versions, "0.9.0") && exists_in(fixture.base.cache, rest));
    spisor_memory_release(versions);
    spisor_memory_release(runtimes);
    finish_runtime(&fixture);
}

static void refuses_a_pinned_version_reaching_outside_its_folder(void) {
    refuses_a_pinned_version_that_is_not_plain("pin-escapes", "../1.2.3", TEST_VERSION);
}

static void refuses_a_pinned_version_of_two_numbers(void) {
    refuses_a_pinned_version_that_is_not_plain("pin-short", "1.2", "1.2");
}

static void refuses_a_hinted_typescript_host_version_outside_its_bundles(void) {
    install_base base;
    start_install_base(&base, "runtime", "hint-escapes");
    write_in(base.cache, "hosts/typescript-host.json", "{\"libraries\":{},\"host\":\"../../escaped\"}");
    write_in(base.cache, "escaped/host.json",
             "{\"node\": {\"pinned\": \"1.2.3\", \"minimum\": \"22.18.0\", \"builds\": {}}}");
    catalog_answer answer = acquire_on(&base, base.path);
    expect_cache_refusal_refused(answer, "is not a version", "hint escapes");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "runtimes"));
    finish_install_base(&base);
}

static void refuses_a_cache_with_no_typescript_host_to_read_the_pin_from(void) {
    install_base base;
    start_install_base(&base, "runtime", "no-layer");
    catalog_answer answer = acquire_on(&base, base.path);
    expect_cache_refusal_refused(answer, "no installed item started on JavaScript carries a Node pin",
                                 "no typescript-host");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "runtimes"));
    finish_install_base(&base);
}

static void refuses_a_typescript_host_whose_host_json_pins_no_node(void) {
    install_base base;
    start_install_base(&base, "runtime", "no-pin");
    write_in(base.cache, "hosts/typescript-host.json", "{\"libraries\":{},\"host\":\"1.0.0\"}");
    write_in(base.cache, "bundles/typescript-host/1.0.0/host.json", "{\"id\": \"typescript-host\", \"runtime\": \"javascript\"}");
    catalog_answer answer = acquire_on(&base, base.path);
    expect_cache_refusal_refused(answer, "no installed item started on JavaScript carries a Node pin",
                                 "no node member");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "runtimes"));
    finish_install_base(&base);
}

/** Every host started on JavaScript runs on one Node, so two installed items pinning different ones are refused. */
static void refuses_two_installed_javascript_items_that_pin_different_nodes_naming_both(void) {
    install_base base;
    start_install_base(&base, "runtime", "two-pins");
    install_pin_of(base.cache, "1.2.3", "file:///nowhere/a.zip", "0000000000000000000000000000000000000000000000000000000000000000");
    write_in(base.cache, "hosts/other-host.json", "{\"libraries\":{},\"host\":\"1.0.0\"}");
    char *manifest = typescript_host_json("1.2.4", MINIMUM, "file:///nowhere/b.zip", "0000000000000000000000000000000000000000000000000000000000000000");
    write_in(base.cache, "bundles/other-host/1.0.0/host.json", manifest);
    spisor_memory_release(manifest);
    catalog_answer answer = acquire_on(&base, base.path);
    expect_cache_refusal_refused(answer, "other-host pins Node 1.2.4 and typescript-host pins Node 1.2.3",
                                 "two pins");
    const char *fix = text_at(answer.tree, "fix");
    CHECK(fix != NULL && strstr(fix, "other-host") != NULL && strstr(fix, "typescript-host") != NULL);
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "runtimes"));
    finish_install_base(&base);
}

/** A hinted host that is not started on JavaScript carries no Node pin, whatever its host.json holds. */
static void reads_no_pin_from_an_item_not_started_on_javascript(void) {
    install_base base;
    start_install_base(&base, "runtime", "native-pin");
    write_in(base.cache, "hosts/fixture-one-host.json", "{\"libraries\":{},\"host\":\"1.0.0\"}");
    write_in(base.cache, "bundles/fixture-one-host/1.0.0/host.json",
             "{\"runtime\": \"native\", \"node\": {\"pinned\": \"1.2.3\", \"minimum\": \"22.18.0\", \"builds\": {}}}");
    catalog_answer answer = acquire_on(&base, base.path);
    expect_cache_refusal_refused(answer, "no installed item started on JavaScript carries a Node pin", "a native pin");
    cJSON_Delete(answer.tree);
    finish_install_base(&base);
}

/** A node on a PATH folder of its own answering version, as node -p process.versions.node does. */
static char *path_node(const install_base *base, const char *version) {
    char *folder = joined(base->root, "path-node");
    write_in(folder, "version.txt", version);
    size_t length = 0;
    uint8_t *probe = spisor_read_file_reporting(RUNTIME_NODE_PROBE_PATH, &length,
                                                &(spisor_read_outcome){SPISOR_READ_FAILED});
#if defined(_WIN32)
    char *node = joined(folder, "node.exe");
#else
    char *node = joined(folder, "node");
#endif
    CHECK(probe != NULL && spisor_replace_file(node, probe, length) == SPISOR_OK);
#if !defined(_WIN32)
    CHECK(chmod(node, 0755) == 0);
#endif
    spisor_memory_release(node);
    spisor_memory_release(probe);
    return folder;
}

static void reuses_a_node_on_the_path_at_the_pinned_minimum(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "path-node", NULL);
    char *folder = path_node(&fixture.base, "22.18.0");
    catalog_answer answer = acquire_on(&fixture.base, folder);
    CHECK(answer.status == SPISOR_OK);
    check_text(text_at(answer.tree, "version"), "22.18.0", "path version");
    check_text(text_at(answer.tree, "source"), "path", "path source");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    const char *executable = text_at(answer.tree, "executable");
    CHECK(executable != NULL && strncmp(executable, folder, strlen(folder)) == 0);
    CHECK(!exists_in(fixture.base.cache, "runtimes/node"));
    cJSON *record = json_in(fixture.base.cache, "runtimes/javascript.json");
    CHECK(record != NULL && cJSON_Compare(record, answer.tree, 1));
    cJSON_Delete(record);
    cJSON_Delete(answer.tree);
    spisor_memory_release(folder);
    finish_runtime(&fixture);
}

static void ignores_a_node_on_the_path_older_than_the_pinned_minimum_and_acquires_the_pin(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "old-path-node", NULL);
    char *folder = path_node(&fixture.base, "22.17.0");
    catalog_answer answer = acquire_on(&fixture.base, folder);
    CHECK(answer.status == SPISOR_OK);
    check_text(text_at(answer.tree, "version"), TEST_VERSION, "pinned version");
    check_text(text_at(answer.tree, "source"), "acquired", "acquired source");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    char *executable = executable_path(&fixture);
    check_text(text_at(answer.tree, "executable"), executable, "pinned executable");
    spisor_memory_release(executable);
    cJSON_Delete(answer.tree);
    spisor_memory_release(folder);
    finish_runtime(&fixture);
}

/** A PATH node reporting a prerelease or nightly of the minimum ranks below it, so the pin is acquired instead. */
static void ignores_a_node_on_the_path_whose_version_is_the_minimum_with_a_suffix(const char *label, const char *version) {
    runtime_fixture fixture;
    start_runtime(&fixture, label, NULL);
    char *folder = path_node(&fixture.base, version);
    catalog_answer answer = acquire_on(&fixture.base, folder);
    CHECK(answer.status == SPISOR_OK);
    check_text(text_at(answer.tree, "source"), "acquired", version);
    cJSON_Delete(answer.tree);
    spisor_memory_release(folder);
    finish_runtime(&fixture);
}

/** A program that would run for half a minute, which a bounded run stops. */
static void stops_a_run_that_does_not_exit_in_time_and_refuses_it(void) {
    cJSON *arguments = cJSON_CreateArray();
#if defined(_WIN32)
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    char narrow[4 * MAX_PATH];
    int written = WideCharToMultiByte(CP_UTF8, 0, folder, (int)length, narrow, (int)sizeof narrow - 1, NULL, NULL);
    narrow[written] = 0;
    char *program = joined(narrow, "ping.exe");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("-n"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("30"));
    cJSON_AddItemToArray(arguments, cJSON_CreateString("127.0.0.1"));
#else
    char *program = joined(spisor_probe_file("/bin/sleep") == SPISOR_FILE_PRESENT ? "/bin" : "/usr/bin", "sleep");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("30"));
#endif
    double started = runtime_now_milliseconds();
    int exit_code = -1;
    char *output = NULL;
    char *problem = NULL;
    CHECK(!runtime_run_to_exit(program, arguments, 500, &exit_code, &output, &problem));
    CHECK(runtime_now_milliseconds() - started < 10000);
    CHECK(problem != NULL && strstr(problem, "in time") != NULL);
    spisor_memory_release(problem);
    spisor_memory_release(output);
    spisor_memory_release(program);
    cJSON_Delete(arguments);
}

#if defined(_WIN32)
static HANDLE held_executable = INVALID_HANDLE_VALUE;

static DWORD WINAPI release_later(LPVOID unused) {
    (void)unused;
    Sleep(400);
    CloseHandle(held_executable);
    held_executable = INVALID_HANDLE_VALUE;
    return 0;
}

/** Opens the staged executable sharing no delete, as a virus scanner reading it does, and closes it shortly after. */
static void hold_staged_executable(const char *staging) {
    char *executable = joined(staging, in_stem(EXECUTABLE_MEMBER));
    wchar_t wide[2048];
    CHECK(MultiByteToWideChar(CP_UTF8, 0, executable, -1, wide, 2048) > 0);
    held_executable = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held_executable != INVALID_HANDLE_VALUE);
    HANDLE thread = CreateThread(NULL, 0, release_later, NULL, 0, NULL);
    CHECK(thread != NULL);
    CloseHandle(thread);
    spisor_memory_release(executable);
}

static void moves_the_runtime_into_place_once_a_scanner_lets_go_of_it(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "scanned", NULL);
    runtime_install_before_publish(hold_staged_executable);
    catalog_answer answer = acquire(&fixture);
    runtime_install_before_publish(NULL);
    CHECK(answer.status == SPISOR_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    char *executable = executable_path(&fixture);
    CHECK(spisor_probe_file(executable) == SPISOR_FILE_PRESENT);
    spisor_memory_release(executable);
    cJSON_Delete(answer.tree);
    while (held_executable != INVALID_HANDLE_VALUE) {
        Sleep(20);
    }
    finish_runtime(&fixture);
}
#endif

/** A program that exits at once leaving a grandchild that holds its output pipe open for half a minute. */
static void returns_when_the_child_exits_though_a_grandchild_holds_its_output(void) {
    cJSON *arguments = cJSON_CreateArray();
    cJSON_AddItemToArray(arguments, cJSON_CreateString("--leave-grandchild"));
    char *program = joined(RUNTIME_NODE_PROBE_PATH, "");
    double started = runtime_now_milliseconds();
    int exit_code = -1;
    char *output = NULL;
    char *problem = NULL;
    CHECK(runtime_run_to_exit(program, arguments, 20000, &exit_code, &output, &problem));
    CHECK(runtime_now_milliseconds() - started < 5000);
    if (exit_code != 0 || problem != NULL) {
        fprintf(stderr, "grandchild run: exit %d, %s\n", exit_code, problem == NULL ? "(no problem)" : problem);
        check_failures++;
    }
    spisor_memory_release(problem);
    spisor_memory_release(output);
    spisor_memory_release(program);
    cJSON_Delete(arguments);
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

static void sweeps_staging_folders_left_over_a_day_and_keeps_a_fresh_one(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "staging-sweep", NULL);
    char old_rest[256];
    char fresh_rest[256];
    snprintf(old_rest, sizeof old_rest, "runtimes/node/" TEST_VERSION "/.%s-staging-1-1", runtime_platform_key());
    snprintf(fresh_rest, sizeof fresh_rest, "runtimes/node/" TEST_VERSION "/.%s-staging-2-1", runtime_platform_key());
    char *old = joined(fixture.base.cache, old_rest);
    char *fresh = joined(fixture.base.cache, fresh_rest);
    write_in(old, "partial", "left by a crash");
    write_in(fresh, "partial", "another install's");
    char *old_file = joined(old, "partial");
    double two_days_ago = runtime_now_milliseconds() - 2 * 24 * 3600000.0;
    set_modified(old_file, two_days_ago);
    set_modified(old, two_days_ago);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_OK);
    cJSON_Delete(answer.tree);
    CHECK(spisor_probe_file(old) != SPISOR_FILE_PRESENT && spisor_probe_file(fresh) == SPISOR_FILE_PRESENT);
    spisor_memory_release(old_file);
    spisor_memory_release(fresh);
    spisor_memory_release(old);
    finish_runtime(&fixture);
}

/** A cache folder deeper than the 260 characters of a Windows MAX_PATH, in segments of 60. */
#define DEEP_CACHE_FOLDER                                                                                     \
    "deep-cache-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/deep-cache-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" \
    "bbbbbbbbbbbbbbbb/deep-cache-cccccccccccccccccccccccccccccccccccccccccccccccccccc/deep-cache-dddddddddddddddddddd" \
    "dddddddddddddddddddddddddddddddddddddddddd/cache"

/** Acquires the runtime into a cache at cache_folder under the fixture's root, the archive served from a plain path. */
static void acquires_into_a_cache_at(const char *label, const char *cache_folder) {
    runtime_fixture fixture;
    start_runtime(&fixture, label, NULL);
    install_base elsewhere = fixture.base;
    elsewhere.cache = joined(fixture.base.root, cache_folder);
    install_node_pin(elsewhere.cache, fixture.pinned_url, fixture.pinned_sha256);
    catalog_answer answer = acquire_on(&elsewhere, elsewhere.path);
    CHECK(answer.status == SPISOR_OK);
    const char *executable = text_at(answer.tree, "executable");
    if (executable == NULL || spisor_probe_file(executable) != SPISOR_FILE_PRESENT) {
        fprintf(stderr, "%s: no runtime executable under %s\n", label, elsewhere.cache);
        check_failures++;
    }
    cJSON_Delete(answer.tree);
    spisor_memory_release(elsewhere.cache);
    finish_runtime(&fixture);
}

static void unpacks_into_a_cache_whose_path_holds_letters_outside_the_ansi_codepage(void) {
    acquires_into_a_cache_at("unicode-cache", "J\xc3\xb6rg/\xce\xa9\xce\xbc\xce\xad\xce\xb3\xce\xb1-\xe6\x97\xa5\xe6\x9c\xac/cache");
}

static void unpacks_into_a_cache_deeper_than_the_windows_path_limit(void) {
    acquires_into_a_cache_at("deep-cache", DEEP_CACHE_FOLDER);
}

int main(void) {
    node_archive_stem(TEST_VERSION, STEM, sizeof STEM);
    snprintf(ARCHIVE_NAME, sizeof ARCHIVE_NAME, "%s" ARCHIVE_ENDING, STEM);
    acquires_the_pinned_runtime_and_answers_its_executable();
    names_the_archive_and_its_folder_from_the_pin_never_from_its_url();
    refuses_a_pinned_version_reaching_outside_its_folder();
    refuses_a_pinned_version_of_two_numbers();
    refuses_a_hinted_typescript_host_version_outside_its_bundles();
    fetches_nothing_on_a_second_run();
    keeps_only_the_executable_and_the_archive_s_hash_in_place_of_the_archive();
    acquires_the_runtime_again_when_its_executable_s_bytes_changed();
    removes_the_runtime_versions_no_longer_pinned();
    refuses_an_archive_whose_hash_differs_from_the_pin_and_leaves_no_executable();
    refuses_a_platform_with_no_pinned_build();
    refuses_a_cache_with_no_typescript_host_to_read_the_pin_from();
    refuses_a_typescript_host_whose_host_json_pins_no_node();
    refuses_two_installed_javascript_items_that_pin_different_nodes_naming_both();
    reads_no_pin_from_an_item_not_started_on_javascript();
    stops_a_run_that_does_not_exit_in_time_and_refuses_it();
    returns_when_the_child_exits_though_a_grandchild_holds_its_output();
    sweeps_staging_folders_left_over_a_day_and_keeps_a_fresh_one();
#if defined(_WIN32)
    moves_the_runtime_into_place_once_a_scanner_lets_go_of_it();
#endif
    reuses_a_node_on_the_path_at_the_pinned_minimum();
    ignores_a_node_on_the_path_older_than_the_pinned_minimum_and_acquires_the_pin();
    ignores_a_node_on_the_path_whose_version_is_the_minimum_with_a_suffix("path-prerelease", "22.18.0-pre");
    ignores_a_node_on_the_path_whose_version_is_the_minimum_with_a_suffix("path-nightly", "22.18.0-nightly20261126");
    unpacks_into_a_cache_whose_path_holds_letters_outside_the_ansi_codepage();
    unpacks_into_a_cache_deeper_than_the_windows_path_limit();
    return check_summary("install_runtime_test");
}
