#include "host_fixture.h"

#include "boot_stub.h"
#include "platform_key.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#define HOST_VERSION "1.0.0"
#define LIBRARY_VERSION "0.1.0"
#define SCRIPT_TIMEOUT 300000
#define HOST_EXECUTABLE "forebay-host" EXECUTABLE_ENDING
#define BOOT_EXECUTABLE "forebay-boot" EXECUTABLE_ENDING
#define LAUNCHER_EXECUTABLE "runtime-launcher.exe"

/** PowerShell started with no console at all answers 0 and runs nothing, so it gets a hidden one. */
#if defined(_WIN32)
#define IS_DETACHABLE 0
#else
#define IS_DETACHABLE 1
#endif

#if defined(_WIN32)
#define SCRIPT_PATH INSTALL_SCRIPTS_PATH "/install.ps1"
#else
#define SCRIPT_PATH INSTALL_SCRIPTS_PATH "/install.sh"
#endif

/** Every tool install.sh calls, linked one by one so the child's PATH holds them and nothing else. */
static const char *const POSIX_TOOLS[] = {"uname", "mkdir", "rm", "mv", "cp", "cat", "chmod", "mktemp", "wc", "tr",
                                          "awk", "curl", "sha256sum", "shasum"};

/** The catalog schema's patterns both scripts were written against; install.ps1 carries them verbatim. */
static const char *const SCHEMA_PATTERNS[][2] = {
    {"definitions/files/propertyNames", "^[^/\\\\:]*[^/\\\\:. ](?:/[^/\\\\:]*[^/\\\\:. ])*(?![\\s\\S])"},
    {"definitions/item/properties/latest",
     "^(?![\\s\\S]*\\.\\.)[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]*[0-9A-Za-z-])?(\\+[0-9A-Za-z.-]*[0-9A-Za-z-])?(?![\\s\\S])"},
    {"definitions/fileRef/properties/sha256", "^[0-9a-f]{64}(?![\\s\\S])"},
};

static char *platform_key_of(const char *name) {
    char rest[256];
    snprintf(rest, sizeof rest, "%s/%s", runtime_platform_key(), name);
    return copy_text(rest);
}

static char *version_directory(const host_world *world) {
    return joined(world->base.cache, "bundles/forebay-host/" HOST_VERSION);
}

static char *cached_file(const host_world *world, const char *name) {
    char rest[512];
    snprintf(rest, sizeof rest, "bundles/forebay-host/" HOST_VERSION "/%s/%s", runtime_platform_key(), name);
    return joined(world->base.cache, rest);
}

static const char *base_name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash > backslash ? slash : backslash;
    return last == NULL ? path : last + 1;
}

/** forebay-host as a build directory catalog build reads: its host.json and this platform's executables. */
static char *stage_forebay_host(const host_world *world) {
    char *stage = joined(world->base.root, "stage/forebay-host");
    char manifest[512];
    snprintf(manifest, sizeof manifest,
             "{\"id\": \"forebay-host\", \"version\": \"" HOST_VERSION "\", \"runtime\": \"native\", \"entry\": \"%s/" HOST_EXECUTABLE "\"}",
             runtime_platform_key());
    write_in(stage, "host.json", manifest);
    char *executable = platform_key_of(HOST_EXECUTABLE);
    char *boot = platform_key_of(BOOT_EXECUTABLE);
    char *executable_path = joined(stage, executable);
    char *boot_path = joined(stage, boot);
    copy_file(FOREBAY_HOST_PATH, executable_path);
    copy_file(FOREBAY_BOOT_PATH, boot_path);
#if defined(_WIN32)
    char *launcher = platform_key_of(LAUNCHER_EXECUTABLE);
    char *launcher_path = joined(stage, launcher);
    copy_file(RUNTIME_LAUNCHER_PATH, launcher_path);
    free(launcher_path);
    free(launcher);
#endif
    free(boot_path);
    free(executable_path);
    free(boot);
    free(executable);
    return stage;
}

static char *stage_runtime_library(const host_world *world) {
    char *stage = joined(world->base.root, "stage/runtime");
    write_in(stage, "library.json", "{\"version\": \"" LIBRARY_VERSION "\"}");
    char *library = joined(stage, base_name_of(RUNTIME_LIBRARY_PATH));
    copy_file(RUNTIME_LIBRARY_PATH, library);
    free(library);
    return stage;
}

/** A local catalog of forebay-host and the runtime library for this platform, written by catalog build. */
static void build_catalog(host_world *world) {
    char *host = stage_forebay_host(world);
    char *library = stage_runtime_library(world);
    char host_argument[1200];
    char library_argument[1200];
    snprintf(host_argument, sizeof host_argument, "%s@%s", host, runtime_platform_key());
    snprintf(library_argument, sizeof library_argument, "runtime=%s@%s", library, runtime_platform_key());
    const char *arguments[] = {"catalog", "build", "--host", host_argument, "--library", library_argument,
                               "--out", world->base.catalog, NULL};
    program run;
    int code = world_run(world, &run, arguments, NULL);
    if (code != 0) {
        fprintf(stderr, "catalog build answered %d: %s\n%s\n", code, run.out, run.err);
        check_failures++;
    }
    program_free(&run);
    free(library);
    free(host);
}

#if defined(_WIN32)

static char *system_folder(const char *rest) {
    const char *root = getenv("SystemRoot");
    char path[1024];
    snprintf(path, sizeof path, "%s%s", root == NULL ? "C:\\Windows" : root, rest);
    return copy_text(path);
}

static char *script_path_variable(const host_world *world, int with_curl) {
    (void)world;
    (void)with_curl;
    char *system32 = system_folder("\\System32");
    char *root = system_folder("");
    char *powershell = system_folder("\\System32\\WindowsPowerShell\\v1.0");
    char path[4096];
    snprintf(path, sizeof path, "%s;%s;%s", system32, root, powershell);
    free(powershell);
    free(root);
    free(system32);
    return copy_text(path);
}

#else

static int is_runnable(const char *path) {
    return access(path, X_OK) == 0;
}

/** The tool's path on this process's PATH, NULL when none holds it. */
static char *tool_on_path(const char *tool) {
    const char *path = getenv("PATH");
    char *copy = copy_text(path == NULL ? "/usr/bin:/bin" : path);
    char *found = NULL;
    for (char *directory = strtok(copy, ":"); directory != NULL && found == NULL; directory = strtok(NULL, ":")) {
        char candidate[2048];
        snprintf(candidate, sizeof candidate, "%s/%s", directory, tool);
        found = is_runnable(candidate) ? copy_text(candidate) : NULL;
    }
    free(copy);
    return found;
}

/** A folder of links to the tools install.sh calls, curl among them only when with_curl is set. */
static char *script_path_variable(const host_world *world, int with_curl) {
    char *folder = joined(world->base.root, with_curl ? "tools" : "tools-without-curl");
    CHECK(spisor_make_directories(folder) == 1);
    for (size_t index = 0; index < sizeof POSIX_TOOLS / sizeof POSIX_TOOLS[0]; index++) {
        char *found = strcmp(POSIX_TOOLS[index], "curl") == 0 && !with_curl ? NULL : tool_on_path(POSIX_TOOLS[index]);
        char *link = joined(folder, POSIX_TOOLS[index]);
        if (found != NULL && !spisor_file_exists(link)) {
            CHECK(symlink(found, link) == 0);
        }
        free(link);
        free(found);
    }
    return folder;
}

#endif

/** Runs the platform's install script with the NULL-ended arguments, curl on its PATH unless with_curl is clear. */
static int run_script(host_world *world, program *run, const char *const *script_arguments, int with_curl) {
    const char *arguments[32];
    size_t count = 0;
#if defined(_WIN32)
    char *executable = system_folder("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    const char *prefix[] = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", SCRIPT_PATH};
#else
    char *executable = copy_text("/bin/sh");
    const char *prefix[] = {SCRIPT_PATH};
#endif
    for (size_t index = 0; index < sizeof prefix / sizeof prefix[0]; index++) {
        arguments[count++] = prefix[index];
    }
    for (; script_arguments != NULL && *script_arguments != NULL && count < 31; script_arguments++) {
        arguments[count++] = *script_arguments;
    }
    arguments[count] = NULL;
    char *path = script_path_variable(world, with_curl);
    world_start(world, run, executable, arguments, path, IS_DETACHABLE);
    int code = program_finish(run, SCRIPT_TIMEOUT);
    free(path);
    free(executable);
    return code;
}

static int install(host_world *world, program *run, const char *const *extra) {
    char *index = index_path_of(&world->base);
    const char *arguments[16] = {index};
    size_t count = 1;
    for (; extra != NULL && *extra != NULL && count < 15; extra++) {
        arguments[count++] = *extra;
    }
    arguments[count] = NULL;
    int code = run_script(world, run, arguments, 1);
    if (code != 0 && (extra == NULL || extra[0] == NULL)) {
        fprintf(stderr, "the script answered %d:\n%s\n%s\n", code, run->out, run->err);
    }
    free(index);
    return code;
}

static int has_same_bytes(const char *path, const char *source) {
    char left[RUNTIME_SHA256_HEX_SIZE];
    char right[RUNTIME_SHA256_HEX_SIZE];
    return spisor_sha256_file(path, left) && spisor_sha256_file(source, right) && strcmp(left, right) == 0;
}

static int fetched_any(const program *run) {
    return (run->out != NULL && strstr(run->out, "fetching ") != NULL) || (run->err != NULL && strstr(run->err, "fetching ") != NULL);
}

static void start_script_world(host_world *world, const char *label) {
    start_world_at(world, "install-script", label);
    build_catalog(world);
}

static void fetches_forebay_host_for_this_platform_and_runs_its_install(void) {
    host_world world;
    start_script_world(&world, "fresh");
    program run;
    CHECK(install(&world, &run, NULL) == 0);
    char fetched[256];
    snprintf(fetched, sizeof fetched, "fetching forebay-host " HOST_VERSION " %s/" HOST_EXECUTABLE, runtime_platform_key());
    expect_contains(run.err, fetched, "names what it fetched");
    char *executable = cached_file(&world, HOST_EXECUTABLE);
    char *boot = cached_file(&world, BOOT_EXECUTABLE);
    CHECK(has_same_bytes(executable, FOREBAY_HOST_PATH));
    CHECK(has_same_bytes(boot, FOREBAY_BOOT_PATH));
#if defined(_WIN32)
    char *launcher = cached_file(&world, LAUNCHER_EXECUTABLE);
    CHECK(has_same_bytes(launcher, RUNTIME_LAUNCHER_PATH));
    free(launcher);
#endif
    CHECK(exists_in(world.base.cache, "bundles/forebay-host/" HOST_VERSION "/.installed.json"));
    char *stub = runtime_boot_stub_path(world.base.cache);
    CHECK(has_same_bytes(stub, FOREBAY_BOOT_PATH));
    char marker[256];
    snprintf(marker, sizeof marker, "libraries/runtime/" LIBRARY_VERSION "/%s/.installed.json", runtime_platform_key());
    CHECK(exists_in(world.base.cache, marker));
    cJSON *registration = registration_in(&world, "forebay-host");
    check_text(text_at(registration, "version"), HOST_VERSION, "registered at the catalog's version");
    cJSON_Delete(registration);
    program status;
    const char *arguments[] = {"forebay-host", "status", NULL};
    world_start(&world, &status, stub, arguments, NULL, 1);
    CHECK(program_finish(&status, 120000) == 0);
    expect_contains(status.out, "\"hosts\"", "forebay-boot runs the installed forebay-host");
    program_free(&status);
    free(stub);
    free(boot);
    free(executable);
    program_free(&run);
    finish_world(&world);
}

static void passes_its_arguments_to_install_and_answers_its_exit_code(void) {
    host_world world;
    start_script_world(&world, "arguments");
    program run;
    const char *absent[] = {"--plugin", "absent-plugin", NULL};
    CHECK(install(&world, &run, absent) == 1);
    expect_contains(run.err, "absent-plugin could not be installed", "install ran with the arguments");
    program_free(&run);
    finish_world(&world);
}

static void fetches_nothing_on_a_second_run(void) {
    host_world world;
    start_script_world(&world, "second");
    program first;
    CHECK(install(&world, &first, NULL) == 0);
    char *files = joined(world.base.catalog, "files");
    char *away = joined(world.base.catalog, "files.away");
    CHECK(rename(files, away) == 0);
    program second;
    CHECK(install(&world, &second, NULL) == 0);
    CHECK(!fetched_any(&second));
    CHECK(rename(away, files) == 0);
    program_free(&second);
    program_free(&first);
    free(away);
    free(files);
    finish_world(&world);
}

static void fetches_forebay_host_again_when_its_bytes_changed(void) {
    host_world world;
    start_script_world(&world, "changed");
    program first;
    CHECK(install(&world, &first, NULL) == 0);
    char *executable = cached_file(&world, HOST_EXECUTABLE);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(executable, &length);
    CHECK(bytes != NULL && length > 0);
    if (bytes != NULL && length > 0) {
        bytes[length - 1] ^= 0xff;
        CHECK(spisor_replace_file(executable, bytes, length) == 1);
    }
    program second;
    CHECK(install(&world, &second, NULL) == 0);
    char fetched[256];
    snprintf(fetched, sizeof fetched, "fetching forebay-host " HOST_VERSION " %s/" HOST_EXECUTABLE, runtime_platform_key());
    expect_contains(second.err, fetched, "fetched again");
    CHECK(has_same_bytes(executable, FOREBAY_HOST_PATH));
    program_free(&second);
    program_free(&first);
    free(bytes);
    free(executable);
    finish_world(&world);
}

static void installs_from_a_catalog_index_that_starts_with_a_byte_order_mark(void) {
    host_world world;
    start_script_world(&world, "byte-order-mark");
    char *text = text_in(world.base.catalog, "catalog.json");
    char *marked = malloc(strlen(text) + 4);
    sprintf(marked, "\xef\xbb\xbf%s", text);
    write_in(world.base.catalog, "with-byte-order-mark.json", marked);
    char *index = joined(world.base.catalog, "with-byte-order-mark.json");
    program run;
    const char *arguments[] = {index, NULL};
    CHECK(run_script(&world, &run, arguments, 1) == 0);
    cJSON *registration = registration_in(&world, "forebay-host");
    check_text(text_at(registration, "version"), HOST_VERSION, "installed");
    cJSON_Delete(registration);
    program_free(&run);
    free(index);
    free(marked);
    free(text);
    finish_world(&world);
}

static void refuses_with_the_fix_when_no_catalog_is_given_and_writes_nothing(void) {
    host_world world;
    start_world_at(&world, "install-script", "no-catalog");
    program run;
    CHECK(run_script(&world, &run, NULL, 1) == 1);
    expect_contains(run.err, "pass the catalog to install from", "the fix");
    CHECK(!spisor_file_exists(world.base.cache));
    program_free(&run);
    finish_world(&world);
}

static void refuses_a_catalog_that_is_neither_an_https_url_nor_a_local_path(void) {
    host_world world;
    start_world_at(&world, "install-script", "scheme");
    program run;
    const char *arguments[] = {"ftp://catalog.test/catalog.json", NULL};
    CHECK(run_script(&world, &run, arguments, 1) == 1);
    expect_contains(run.err, "is neither an https url nor a local path", "names the scheme");
    CHECK(!spisor_file_exists(world.base.cache));
    program_free(&run);
    finish_world(&world);
}

typedef void (*catalog_change)(host_world *world, cJSON *files);

/** A catalog change the scripts must refuse, and the refusal's words, %s standing for the platform key. */
typedef struct hostile_catalog {
    const char *name;
    catalog_change change;
    const char *refusal;
} hostile_catalog;

static cJSON *platform_files(cJSON *catalog) {
    cJSON *version = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(catalog, "items"), "forebay-host"),
                                         "versions"),
        HOST_VERSION);
    cJSON *platform = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(version, "platforms"), runtime_platform_key());
    return cJSON_GetObjectItemCaseSensitive(platform, "files");
}

static cJSON *file_named(cJSON *files, const char *name) {
    char *key = platform_key_of(name);
    cJSON *file = cJSON_GetObjectItemCaseSensitive(files, key);
    free(key);
    return file;
}

static void remove_key(cJSON *files, const char *name) {
    char *key = platform_key_of(name);
    cJSON_DeleteItemFromObjectCaseSensitive(files, key);
    free(key);
}

static void set_member(cJSON *file, const char *member, cJSON *value) {
    cJSON_ReplaceItemInObjectCaseSensitive(file, member, value);
}

static void copy_manifest_as(cJSON *files, const char *key) {
    cJSON *source = cJSON_GetObjectItemCaseSensitive(files, "host.json");
    cJSON_AddItemToObject(files, key, cJSON_Duplicate(source, 1));
}

static void without_the_executable(host_world *world, cJSON *files) {
    (void)world;
    remove_key(files, HOST_EXECUTABLE);
}

static void without_forebay_boot(host_world *world, cJSON *files) {
    (void)world;
    remove_key(files, BOOT_EXECUTABLE);
}

#if defined(_WIN32)
static void without_runtime_launcher(host_world *world, cJSON *files) {
    (void)world;
    remove_key(files, LAUNCHER_EXECUTABLE);
}
#endif

static void without_this_platform(host_world *world, cJSON *files) {
    (void)world;
    while (files->child != NULL) {
        cJSON_Delete(cJSON_DetachItemViaPointer(files, files->child));
    }
}

static void with_a_key_that_climbs_out(host_world *world, cJSON *files) {
    (void)world;
    copy_manifest_as(files, "../escape");
}

static void with_a_url_outside_the_catalog_directory(host_world *world, cJSON *files) {
    (void)world;
    set_member(file_named(files, HOST_EXECUTABLE), "url", cJSON_CreateString("../outside/" HOST_EXECUTABLE));
}

static void with_a_hash_the_bytes_do_not_have(host_world *world, cJSON *files) {
    (void)world;
    set_member(file_named(files, HOST_EXECUTABLE), "sha256",
               cJSON_CreateString("0000000000000000000000000000000000000000000000000000000000000000"));
}

static void with_a_file_that_cannot_be_fetched(host_world *world, cJSON *files) {
    (void)world;
    set_member(file_named(files, HOST_EXECUTABLE), "url", cJSON_CreateString("not-served/" HOST_EXECUTABLE));
}

static void with_a_file_served_cut_short(host_world *world, cJSON *files) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(FOREBAY_HOST_PATH, &length);
    char *path = joined(world->base.catalog, "cut-short/" HOST_EXECUTABLE);
    char *parent = joined(path, "..");
    CHECK(bytes != NULL && spisor_make_directories(parent) == 1 && spisor_replace_file(path, bytes, length / 2) == 1);
    set_member(file_named(files, HOST_EXECUTABLE), "url", cJSON_CreateString("cut-short/" HOST_EXECUTABLE));
    free(parent);
    free(path);
    free(bytes);
}

static void with_a_size_its_bytes_do_not_have(host_world *world, cJSON *files) {
    (void)world;
    cJSON *file = file_named(files, HOST_EXECUTABLE);
    double size = cJSON_GetObjectItemCaseSensitive(file, "size")->valuedouble;
    set_member(file, "size", cJSON_CreateNumber(size + 1));
}

static void with_a_key_ending_in_a_dot(host_world *world, cJSON *files) {
    (void)world;
    copy_manifest_as(files, "host.json.");
}

static void with_two_keys_that_differ_only_by_case(host_world *world, cJSON *files) {
    (void)world;
    copy_manifest_as(files, "HOST.json");
}

static void with_a_key_naming_the_completion_marker(host_world *world, cJSON *files) {
    (void)world;
    copy_manifest_as(files, ".installed.json");
}

static void with_a_size_that_is_not_a_whole_number(host_world *world, cJSON *files) {
    (void)world;
    set_member(cJSON_GetObjectItemCaseSensitive(files, "host.json"), "size", cJSON_CreateNumber(1.5));
}

static const hostile_catalog HOSTILE_CATALOGS[] = {
    {"a forebay-host that lists no executable for this platform", without_the_executable, "without %s/" HOST_EXECUTABLE},
    {"a forebay-host that lists no forebay-boot", without_forebay_boot, "without %s/" BOOT_EXECUTABLE},
#if defined(_WIN32)
    {"a forebay-host that lists no runtime-launcher.exe", without_runtime_launcher, "without %s/" LAUNCHER_EXECUTABLE},
#endif
    {"a forebay-host with no build for this platform", without_this_platform, "lists no forebay-host " HOST_VERSION " build for %s"},
    {"a file key that climbs out of the version", with_a_key_that_climbs_out, "file \"../escape\" with a name, url"},
    {"a file url outside the catalog's directory", with_a_url_outside_the_catalog_directory, "file \"%s/" HOST_EXECUTABLE "\" with a name, url"},
    {"a file whose bytes do not have the listed hash", with_a_hash_the_bytes_do_not_have, "bytes and SHA-256 0000"},
    {"a file whose bytes do not have the listed size", with_a_size_its_bytes_do_not_have, "does not have the "},
    {"a file that cannot be fetched", with_a_file_that_cannot_be_fetched, "could not fetch "},
    {"a file served cut short", with_a_file_served_cut_short, "cut-short/" HOST_EXECUTABLE " does not have the "},
    {"a file key the schema refuses for ending in a dot", with_a_key_ending_in_a_dot, "file \"host.json.\" with a name, url"},
    {"two file keys that differ only by case", with_two_keys_that_differ_only_by_case, "host.json and HOST.json, which differ only by case"},
    {"a file key naming the cache's completion marker", with_a_key_naming_the_completion_marker, "file \".installed.json\" with a name, url"},
    {"a file size that is not a whole number", with_a_size_that_is_not_a_whole_number, "file \"host.json\" with a name, url"},
};

static char *hostile_index(host_world *world, const hostile_catalog *hostile, size_t number) {
    cJSON *catalog = json_in(world->base.catalog, "catalog.json");
    hostile->change(world, platform_files(catalog));
    char name[64];
    snprintf(name, sizeof name, "hostile-%zu.json", number);
    char *text = cJSON_Print(catalog);
    write_in(world->base.catalog, name, text);
    cJSON_free(text);
    cJSON_Delete(catalog);
    return joined(world->base.catalog, name);
}

/** The fake application where an earlier forebay-host would be, so running it leaves install.started behind. */
static void plant_stale_host(host_world *world) {
    char *stale = cached_file(world, HOST_EXECUTABLE);
    copy_file(FAKE_APPLICATION_PATH, stale);
    write_in(world->signals, "install.release", "");
    free(stale);
}

static int is_one_of(const char *name, const char *const *allowed, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(name, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void expect_only(const char *directory, const char *const *allowed, size_t count, const char *label) {
    runtime_directory_listing listing;
    if (!spisor_file_exists(directory) || !runtime_list_directory(directory, &listing)) {
        return;
    }
    for (size_t index = 0; index < listing.count; index++) {
        if (!is_one_of(listing.entries[index].name, allowed, count)) {
            fprintf(stderr, "%s: %s is left in %s\n", label, listing.entries[index].name, directory);
            check_failures++;
        }
    }
    runtime_free_directory_listing(&listing);
}

static void expect_no_strays(const host_world *world, const char *label) {
    char *version = version_directory(world);
    char *platform = joined(version, runtime_platform_key());
    const char *version_names[] = {"host.json", runtime_platform_key()};
    const char *platform_names[] = {HOST_EXECUTABLE, BOOT_EXECUTABLE, LAUNCHER_EXECUTABLE};
    expect_only(version, version_names, 2, label);
    expect_only(platform, platform_names, 3, label);
    free(platform);
    free(version);
}

static void refuses_each_hostile_catalog_and_runs_nothing(void) {
    host_world world;
    start_script_world(&world, "hostile");
    for (size_t number = 0; number < sizeof HOSTILE_CATALOGS / sizeof HOSTILE_CATALOGS[0]; number++) {
        const hostile_catalog *hostile = &HOSTILE_CATALOGS[number];
        CHECK(runtime_remove_tree(world.base.cache) == 1);
        plant_stale_host(&world);
        char *index = hostile_index(&world, hostile, number);
        program run;
        const char *arguments[] = {index, NULL};
        int code = run_script(&world, &run, arguments, 1);
        if (code != 1) {
            fprintf(stderr, "%s: the script answered %d:\n%s\n%s\n", hostile->name, code, run.out, run.err);
            check_failures++;
        }
        char refusal[512];
        snprintf(refusal, sizeof refusal, hostile->refusal, runtime_platform_key());
        expect_contains(run.err, refusal, hostile->name);
        expect_contains(run.err, "fix: ", hostile->name);
        CHECK(!exists_in(world.signals, "install.started"));
        CHECK(!exists_in(world.base.cache, "bundles/forebay-host/" HOST_VERSION "/.installed.json"));
        CHECK(!exists_in(world.base.cache, "libraries"));
        CHECK(!exists_in(world.base.cache, "bundles/forebay-host/escape"));
        expect_no_strays(&world, hostile->name);
        program_free(&run);
        free(index);
    }
    finish_world(&world);
}

#if defined(_WIN32)

static void leaves_no_preference_or_variable_in_the_shell_that_runs_it_through_invoke_expression(void) {
    host_world world;
    start_world_at(&world, "install-script", "invoke-expression");
    char command[2048];
    snprintf(command, sizeof command,
             "$ErrorActionPreference = 'Continue'; Invoke-Expression ([System.IO.File]::ReadAllText('%s')); "
             "Write-Output ('{0}|{1}|{2}' -f $ErrorActionPreference, (Test-Path variable:HostId), $LASTEXITCODE)",
             SCRIPT_PATH);
    char *executable = system_folder("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
    const char *arguments[] = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command, NULL};
    char *path = script_path_variable(&world, 1);
    program run;
    world_start(&world, &run, executable, arguments, path, IS_DETACHABLE);
    CHECK(program_finish(&run, SCRIPT_TIMEOUT) == 0);
    expect_contains(run.out, "Continue|False|1", "the shell is left as it was");
    program_free(&run);
    free(path);
    free(executable);
    finish_world(&world);
}

#else

static void names_curl_as_the_one_thing_to_install_when_it_is_missing(void) {
    host_world world;
    start_script_world(&world, "without-curl");
    char *index = index_path_of(&world.base);
    program run;
    const char *arguments[] = {index, NULL};
    CHECK(run_script(&world, &run, arguments, 0) == 1);
    expect_contains(run.err, "install curl, then run this again", "names curl");
    program_free(&run);
    free(index);
    finish_world(&world);
}

#endif

static cJSON *schema_pattern(const cJSON *schema, const char *path) {
    char *copy = copy_text(path);
    const cJSON *node = schema;
    for (char *part = strtok(copy, "/"); part != NULL; part = strtok(NULL, "/")) {
        node = cJSON_GetObjectItemCaseSensitive(node, part);
    }
    free(copy);
    return cJSON_GetObjectItemCaseSensitive(node, "pattern");
}

static void both_scripts_check_the_patterns_the_catalog_schema_holds(void) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(CATALOG_SCHEMA_PATH, &length);
    cJSON *schema = bytes == NULL ? NULL : cJSON_ParseWithLength((const char *)bytes, length);
    char *powershell = read_text_file(INSTALL_SCRIPTS_PATH "/install.ps1");
    for (size_t index = 0; index < sizeof SCHEMA_PATTERNS / sizeof SCHEMA_PATTERNS[0]; index++) {
        const cJSON *pattern = schema_pattern(schema, SCHEMA_PATTERNS[index][0]);
        if (!cJSON_IsString(pattern) || strcmp(pattern->valuestring, SCHEMA_PATTERNS[index][1]) != 0) {
            fprintf(stderr, "the schema's %s is no longer the pattern install.sh and install.ps1 check; change both scripts with it\n",
                    SCHEMA_PATTERNS[index][0]);
            check_failures++;
        }
        expect_contains(powershell, SCHEMA_PATTERNS[index][1], "install.ps1 carries the schema's pattern");
    }
    free(powershell);
    cJSON_Delete(schema);
    free(bytes);
}

static void expect_ascii(const char *path) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    CHECK(bytes != NULL && length > 0);
    for (size_t index = 0; bytes != NULL && index < length; index++) {
        if (!(bytes[index] == '\t' || bytes[index] == '\n' || (bytes[index] >= 0x20 && bytes[index] <= 0x7e))) {
            fprintf(stderr, "%s holds byte 0x%02x at %zu, outside printable ASCII\n", path, bytes[index], index);
            check_failures++;
            break;
        }
    }
    free(bytes);
}

static void both_scripts_hold_nothing_outside_printable_ascii(void) {
    expect_ascii(INSTALL_SCRIPTS_PATH "/install.sh");
    expect_ascii(INSTALL_SCRIPTS_PATH "/install.ps1");
}

int main(void) {
    user_path_guard guard;
    guard_user_path(&guard);
    both_scripts_check_the_patterns_the_catalog_schema_holds();
    both_scripts_hold_nothing_outside_printable_ascii();
    fetches_forebay_host_for_this_platform_and_runs_its_install();
    passes_its_arguments_to_install_and_answers_its_exit_code();
    fetches_nothing_on_a_second_run();
    fetches_forebay_host_again_when_its_bytes_changed();
    installs_from_a_catalog_index_that_starts_with_a_byte_order_mark();
    refuses_with_the_fix_when_no_catalog_is_given_and_writes_nothing();
    refuses_a_catalog_that_is_neither_an_https_url_nor_a_local_path();
    refuses_each_hostile_catalog_and_runs_nothing();
#if defined(_WIN32)
    leaves_no_preference_or_variable_in_the_shell_that_runs_it_through_invoke_expression();
#else
    names_curl_as_the_one_thing_to_install_when_it_is_missing();
#endif
    expect_user_path_unchanged(&guard);
    return check_summary("install_script_test");
}
