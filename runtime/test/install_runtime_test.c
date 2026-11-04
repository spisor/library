#include "install_fixture.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "install_hooks.h"
#include "file_times.h"
#include "install_runtime.h"
#include "process_run.h"

#define TEST_VERSION "1.2.3"
#define STEM "node-v1.2.3-fixture"

#if defined(_WIN32)
#define ARCHIVE_NAME STEM ".zip"
#define EXECUTABLE_MEMBER "node.exe"
#else
#define ARCHIVE_NAME STEM ".tar.gz"
#define EXECUTABLE_MEMBER "bin/node"
#endif

typedef struct runtime_fixture {
    install_base base;
    char *archive;
    char archive_sha256[RUNTIME_SHA256_HEX_SIZE];
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
    const char *tar = spisor_file_exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar";
    char *copy = malloc(strlen(tar) + 1);
    strcpy(copy, tar);
    return copy;
#endif
}

/** Packs a runtime archive holding the executable beside a README and a lib folder, as Node's archives do. */
static void pack_archive(runtime_fixture *fixture, const char *executable_content) {
    char *packing = joined(fixture->base.root, "packing");
    char *stem = joined(packing, STEM);
    write_in(stem, EXECUTABLE_MEMBER, executable_content);
    write_in(stem, "README.md", "readme");
    write_in(stem, "lib/node_modules/npm/package.json", "{}");
    fixture->archive = joined(fixture->base.root, "served/" ARCHIVE_NAME);
    char *served = joined(fixture->base.root, "served");
    CHECK(spisor_make_directories(served) == 1);
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
    cJSON_AddItemToArray(arguments, cJSON_CreateString(STEM));
    char *tar = system_tar();
    int exit_code = -1;
    char *problem = NULL;
    CHECK(runtime_run_to_exit(tar, arguments, 120000, &exit_code, NULL, &problem) && exit_code == 0);
    CHECK(spisor_sha256_file(fixture->archive, fixture->archive_sha256));
    free(problem);
    free(tar);
    cJSON_Delete(arguments);
    free(served);
    free(stem);
    free(packing);
}

static void start_runtime(runtime_fixture *fixture, const char *label, const char *sha256_override) {
    start_install_base(&fixture->base, "runtime", label);
    pack_archive(fixture, "the node executable");
    char *url = runtime_catalog_file_url_of_path(RUNTIME_HOST_IS_WINDOWS, fixture->archive);
    runtime_install_use_runtime_build(TEST_VERSION, url, sha256_override == NULL ? fixture->archive_sha256 : sha256_override);
    free(url);
}

static void finish_runtime(runtime_fixture *fixture) {
    runtime_install_use_runtime_build(NULL, NULL, NULL);
    free(fixture->archive);
    finish_install_base(&fixture->base);
}

/** install.runtime for base's cache, looking for a node on path. */
static catalog_answer acquire_on(const install_base *base, const char *path) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", base->cache);
    cJSON_AddItemToObject(argument, "environment", environment_with_path(base, path));
    catalog_answer answer = call_catalog(runtime_install_runtime, argument);
    if (answer.status != SPISOR_STATUS_SUCCESS) {
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
    char *path = joined(directory, STEM "/" EXECUTABLE_MEMBER);
    free(directory);
    return path;
}

static void acquires_the_pinned_runtime_and_answers_its_executable(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "acquires", NULL);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    char *executable = executable_path(&fixture);
    char expected[2048];
    cJSON *quoted = cJSON_CreateString(executable);
    char *executable_json = cJSON_PrintUnformatted(quoted);
    snprintf(expected, sizeof expected, "{\"name\":\"node\",\"version\":\"" TEST_VERSION "\",\"executable\":%s,\"fetched\":true,\"source\":\"acquired\"}",
             executable_json);
    expect_json(answer.tree, expected, "acquired");
    CHECK(spisor_file_exists(executable));
    cJSON_free(executable_json);
    cJSON_Delete(quoted);
    cJSON_Delete(answer.tree);
    free(executable);
    finish_runtime(&fixture);
}

static void fetches_nothing_on_a_second_run(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "second", NULL);
    cJSON_Delete(acquire(&fixture).tree);
    CHECK(runtime_remove_file_entry(fixture.archive) == RUNTIME_ENTRY_DONE);
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
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
    CHECK(!exists_in(directory, STEM "/README.md") && !exists_in(directory, STEM "/lib"));
    char executable_sha256[RUNTIME_SHA256_HEX_SIZE];
    CHECK(spisor_sha256_file(executable, executable_sha256));
    char expected[200];
    snprintf(expected, sizeof expected, "%s\n%s\n", fixture.archive_sha256, executable_sha256);
    char *marker = text_in(directory, ".unpacked");
    check_text(marker, expected, "unpacked marker");
    runtime_directory_listing listing;
    CHECK(runtime_list_directory(directory, &listing) == 1);
    size_t visible = 0;
    for (size_t index = 0; index < listing.count; index++) {
        if (listing.entries[index].name[0] != '.') {
            visible++;
            CHECK(strcmp(listing.entries[index].name, STEM) == 0);
        }
    }
    CHECK(visible == 1);
    runtime_free_directory_listing(&listing);
    free(marker);
    free(executable);
    free(directory);
    finish_runtime(&fixture);
}

static void acquires_the_runtime_again_when_its_executable_s_bytes_changed(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "changed", NULL);
    cJSON_Delete(acquire(&fixture).tree);
    char *executable = executable_path(&fixture);
    write_file(executable, "altered bytes");
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(executable, &length);
    CHECK(bytes != NULL && length == strlen("the node executable") && memcmp(bytes, "the node executable", length) == 0);
    free(bytes);
    cJSON_Delete(answer.tree);
    free(executable);
    finish_runtime(&fixture);
}

static void removes_the_runtime_versions_no_longer_pinned(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "stale", NULL);
    char rest[256];
    snprintf(rest, sizeof rest, "runtimes/node/1.0.0/%s/node", runtime_platform_key());
    write_in(fixture.base.cache, rest, "an older pin");
    catalog_answer answer = acquire(&fixture);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(fixture.base.cache, "runtimes/node/1.0.0"));
    char *executable = executable_path(&fixture);
    CHECK(spisor_file_exists(executable));
    free(executable);
    finish_runtime(&fixture);
}

static void refuses_an_archive_whose_hash_differs_from_the_pin_and_leaves_no_executable(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "mismatch", "0000000000000000000000000000000000000000000000000000000000000000");
    catalog_answer answer = acquire(&fixture);
    expect_refusal(answer, NULL, "hash mismatch");
    cJSON_Delete(answer.tree);
    char *directory = runtime_directory(&fixture);
    char *executable = executable_path(&fixture);
    CHECK(!spisor_file_exists(executable) && !exists_in(directory, ".unpacked") && !exists_in(directory, ARCHIVE_NAME));
    free(executable);
    free(directory);
    finish_runtime(&fixture);
}

static void refuses_a_platform_with_no_pinned_build(void) {
    install_base base;
    start_install_base(&base, "runtime", "unpinned");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "cache", base.cache);
    cJSON_AddItemToObject(argument, "environment", environment_of(&base));
    cJSON_AddStringToObject(argument, "platform", "aix-ppc64");
    catalog_answer answer = call_catalog(runtime_install_runtime, argument);
    expect_refusal(answer, "aix-ppc64", "no pinned build");
    cJSON_Delete(answer.tree);
    CHECK(!exists_in(base.cache, "runtimes"));
    finish_install_base(&base);
}

/** A node on a PATH folder of its own answering version, as node -p process.versions.node does. */
static char *path_node(const install_base *base, const char *version) {
    char *folder = joined(base->root, "path-node");
    write_in(folder, "version.txt", version);
    size_t length = 0;
    uint8_t *probe = spisor_read_file(RUNTIME_NODE_PROBE_PATH, &length);
#if defined(_WIN32)
    char *node = joined(folder, "node.exe");
#else
    char *node = joined(folder, "node");
#endif
    CHECK(probe != NULL && spisor_replace_file(node, probe, length) == 1);
#if !defined(_WIN32)
    CHECK(chmod(node, 0755) == 0);
#endif
    free(node);
    free(probe);
    return folder;
}

static void reuses_a_node_on_the_path_whose_major_is_twenty_or_more(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "path-node", NULL);
    char *folder = path_node(&fixture.base, "22.1.0");
    catalog_answer answer = acquire_on(&fixture.base, folder);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "version"), "22.1.0", "path version");
    check_text(text_at(answer.tree, "source"), "path", "path source");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    const char *executable = text_at(answer.tree, "executable");
    CHECK(executable != NULL && strncmp(executable, folder, strlen(folder)) == 0);
    CHECK(!exists_in(fixture.base.cache, "runtimes/node"));
    cJSON *record = json_in(fixture.base.cache, "runtimes/javascript.json");
    CHECK(record != NULL && cJSON_Compare(record, answer.tree, 1));
    cJSON_Delete(record);
    cJSON_Delete(answer.tree);
    free(folder);
    finish_runtime(&fixture);
}

static void ignores_a_node_on_the_path_older_than_twenty_and_acquires_the_pin(void) {
    runtime_fixture fixture;
    start_runtime(&fixture, "old-path-node", NULL);
    char *folder = path_node(&fixture.base, "18.0.0");
    catalog_answer answer = acquire_on(&fixture.base, folder);
    CHECK(answer.status == SPISOR_STATUS_SUCCESS);
    check_text(text_at(answer.tree, "version"), TEST_VERSION, "pinned version");
    check_text(text_at(answer.tree, "source"), "acquired", "acquired source");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "fetched")));
    char *executable = executable_path(&fixture);
    check_text(text_at(answer.tree, "executable"), executable, "pinned executable");
    free(executable);
    cJSON_Delete(answer.tree);
    free(folder);
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
    char *program = joined(spisor_file_exists("/bin/sleep") ? "/bin" : "/usr/bin", "sleep");
    cJSON_AddItemToArray(arguments, cJSON_CreateString("30"));
#endif
    double started = runtime_now_milliseconds();
    int exit_code = -1;
    char *output = NULL;
    char *problem = NULL;
    CHECK(!runtime_run_to_exit(program, arguments, 500, &exit_code, &output, &problem));
    CHECK(runtime_now_milliseconds() - started < 10000);
    CHECK(problem != NULL && strstr(problem, "in time") != NULL);
    free(problem);
    free(output);
    free(program);
    cJSON_Delete(arguments);
}

int main(void) {
    acquires_the_pinned_runtime_and_answers_its_executable();
    fetches_nothing_on_a_second_run();
    keeps_only_the_executable_and_the_archive_s_hash_in_place_of_the_archive();
    acquires_the_runtime_again_when_its_executable_s_bytes_changed();
    removes_the_runtime_versions_no_longer_pinned();
    refuses_an_archive_whose_hash_differs_from_the_pin_and_leaves_no_executable();
    refuses_a_platform_with_no_pinned_build();
    stops_a_run_that_does_not_exit_in_time_and_refuses_it();
    reuses_a_node_on_the_path_whose_major_is_twenty_or_more();
    ignores_a_node_on_the_path_older_than_twenty_and_acquires_the_pin();
    return check_summary("install_runtime_test");
}
