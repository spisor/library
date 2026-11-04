#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "install_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_text.h"
#include "directory_tree.h"
#include "fetch.h"
#include "file_times.h"
#include "install_support.h"
#include "json_service.h"
#include "on_path.h"
#include "paths.h"
#include "platform_key.h"
#include "process_run.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#include "windows_file.h"
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

#ifdef SPISOR_INSTALL_TEST_HOOKS
#include "install_hooks.h"
#endif

#define RUNTIME_NAME "node"
#define PINNED_VERSION "26.13.0"
#define COMPATIBLE_MAJOR 20
#define UNPACKED_MARKER ".unpacked"
#define RECORD_FILE "javascript.json"
#define DOWNLOAD_SECONDS 1800
#define TAR_MILLISECONDS 600000u
#define VERSION_MILLISECONDS 10000u
#define RUNTIME_FIX "install the ecosystem on win32-x64, linux-x64, darwin-x64 or darwin-arm64, or put Node 20 or newer on the PATH"
#define AGAIN_FIX "run install again; if it repeats, report the pinned archive"
#define CACHE_FIX "check that SPISOR_CACHE names a writable folder, then run install again"
#define STALE_STAGING_MILLISECONDS (24.0 * 3600000.0)
#define ZIP_ENDING ".zip"
#define TAR_GZ_ENDING ".tar.gz"

typedef struct runtime_build {
    const char *platform;
    const char *url;
    const char *sha256;
} runtime_build;

static const runtime_build PINNED_BUILDS[] = {
    {"win32-x64", "https://nodejs.org/dist/v26.13.0/node-v26.13.0-win-x64.zip",
     "dcc60d59a0746999f99064a01c020445d54d84228c691cdbf25b49ac3b59150a"},
    {"linux-x64", "https://nodejs.org/dist/v26.13.0/node-v26.13.0-linux-x64.tar.gz",
     "5848553868b402d3fabb5b09270649977780a390f832155721025a08b5104455"},
    {"darwin-x64", "https://nodejs.org/dist/v26.13.0/node-v26.13.0-darwin-x64.tar.gz",
     "525c9b29bb0263d687de60a4cec0e2a6db876a8eb032369e5795eca71e7c358e"},
    {"darwin-arm64", "https://nodejs.org/dist/v26.13.0/node-v26.13.0-darwin-arm64.tar.gz",
     "d38bd5e5a2ddce6bbb5ce45e46e208337da4d51fffe611e567fd32073d6439f6"},
};

/** Where one unpacked runtime lives: its folder, the archive fetched into it, its marker and its executable. */
typedef struct runtime_layout {
    char *directory;
    char *archive;
    char *marker;
    char *executable;
} runtime_layout;

/** One acquisition: the build chosen, the folder it is published to and the staging folder it is built in. */
typedef struct acquisition {
    const char *version;
    runtime_build build;
    int zip;
    const char *archive_name;
    char *versions;
    char *stem;
    char *member;
    runtime_layout final;
    runtime_layout staged;
} acquisition;

#ifdef SPISOR_INSTALL_TEST_HOOKS
static char *test_version = NULL;
static char *test_url = NULL;
static char *test_sha256 = NULL;
static runtime_build test_build = {NULL, NULL, NULL};

void runtime_install_use_runtime_build(const char *version, const char *url, const char *sha256) {
    free(test_version);
    free(test_url);
    free(test_sha256);
    test_version = url == NULL ? NULL : runtime_catalog_copy(version);
    test_url = url == NULL ? NULL : runtime_catalog_copy(url);
    test_sha256 = url == NULL ? NULL : runtime_catalog_copy(sha256);
    test_build.url = test_url;
    test_build.sha256 = test_sha256;
}
#endif

#ifdef SPISOR_INSTALL_TEST_HOOKS
static void (*publish_hook)(const char *staging) = NULL;

void runtime_install_before_publish(void (*hook)(const char *staging)) {
    publish_hook = hook;
}
#endif

static void before_publish(const acquisition *runtime) {
#ifdef SPISOR_INSTALL_TEST_HOOKS
    if (publish_hook != NULL) {
        publish_hook(runtime->staged.directory);
    }
#else
    (void)runtime;
#endif
}

static int build_for(const char *platform, runtime_build *build, const char **version) {
#ifdef SPISOR_INSTALL_TEST_HOOKS
    if (test_build.url != NULL) {
        *build = test_build;
        build->platform = platform;
        *version = test_version;
        return 1;
    }
#endif
    *version = PINNED_VERSION;
    for (size_t index = 0; index < sizeof PINNED_BUILDS / sizeof PINNED_BUILDS[0]; index++) {
        if (strcmp(PINNED_BUILDS[index].platform, platform) == 0) {
            *build = PINNED_BUILDS[index];
            return 1;
        }
    }
    return 0;
}

static int ends_with(const char *text, const char *ending) {
    size_t length = strlen(text);
    size_t ending_length = strlen(ending);
    return length >= ending_length && strcmp(text + length - ending_length, ending) == 0;
}

static char *joined(const char *const *segments, size_t count) {
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, count);
}

static cJSON *refused(const char *message, const char *subject, const char *fix) {
    const char *parts[2] = {message, subject};
    return runtime_cache_refusal(parts, 2, fix);
}

static void discard_layout(runtime_layout *layout) {
    free(layout->directory);
    free(layout->archive);
    free(layout->marker);
    free(layout->executable);
}

static void discard(acquisition *runtime) {
    free(runtime->versions);
    free(runtime->stem);
    free(runtime->member);
    discard_layout(&runtime->final);
    discard_layout(&runtime->staged);
}

/** Lays out the runtime in directory, which it takes. 1 on success. */
static int lay_out_in(const acquisition *runtime, char *directory, runtime_layout *layout) {
    layout->directory = directory;
    const char *archive_segments[2] = {directory, runtime->archive_name};
    const char *marker_segments[2] = {directory, UNPACKED_MARKER};
    const char *executable_segments[3] = {directory, runtime->stem, runtime->zip ? "node.exe" : "bin/node"};
    layout->archive = directory == NULL ? NULL : joined(archive_segments, 2);
    layout->marker = directory == NULL ? NULL : joined(marker_segments, 2);
    layout->executable = directory == NULL ? NULL : joined(executable_segments, 3);
    return layout->archive != NULL && layout->marker != NULL && layout->executable != NULL;
}

static unsigned long next_acquisition_number(void) {
#if defined(_WIN32)
    static volatile LONG acquisitions = 0;
    return (unsigned long)InterlockedIncrement(&acquisitions);
#else
    static unsigned long acquisitions = 0;
    return __atomic_add_fetch(&acquisitions, 1, __ATOMIC_RELAXED);
#endif
}

/** @implNote Named by process and a number no other acquisition of this process takes, so two installs never build in one folder. */
static char *staging_folder(const char *cache, const char *version, const char *platform) {
    char name[128];
    snprintf(name, sizeof name, ".%s-staging-%d-%lu", platform, (int)current_process_id(), next_acquisition_number());
    const char *segments[5] = {cache, "runtimes", RUNTIME_NAME, version, name};
    return joined(segments, 5);
}

static int lay_out(acquisition *runtime, const char *cache, const char *platform) {
    const char *archive_name = strrchr(runtime->build.url, '/');
    runtime->archive_name = archive_name == NULL ? runtime->build.url : archive_name + 1;
    const char *ending = runtime->zip ? ZIP_ENDING : TAR_GZ_ENDING;
    runtime->stem = runtime_catalog_copy_bytes(runtime->archive_name, strlen(runtime->archive_name) - strlen(ending));
    const char *member_parts[2] = {runtime->stem, runtime->zip ? "/node.exe" : "/bin/node"};
    runtime->member = runtime->stem == NULL ? NULL : runtime_cache_text(member_parts, 2);
    const char *versions_segments[3] = {cache, "runtimes", RUNTIME_NAME};
    const char *directory_segments[5] = {cache, "runtimes", RUNTIME_NAME, runtime->version, platform};
    runtime->versions = joined(versions_segments, 3);
    return runtime->member != NULL && runtime->versions != NULL &&
           lay_out_in(runtime, joined(directory_segments, 5), &runtime->final) &&
           lay_out_in(runtime, staging_folder(cache, runtime->version, platform), &runtime->staged);
}

/** Whether layout's marker names the pinned archive and its executable still has the hash the marker names. */
static int is_unpacked(const acquisition *runtime, const runtime_layout *layout) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(layout->marker, &length);
    char recorded[2 * RUNTIME_SHA256_HEX_SIZE + 2];
    char actual[RUNTIME_SHA256_HEX_SIZE];
    int unpacked = bytes != NULL && length == 2 * (RUNTIME_SHA256_HEX_SIZE - 1) + 2;
    if (unpacked) {
        memcpy(recorded, bytes, length);
        recorded[length] = 0;
        unpacked = strncmp(recorded, runtime->build.sha256, RUNTIME_SHA256_HEX_SIZE - 1) == 0 &&
                   recorded[RUNTIME_SHA256_HEX_SIZE - 1] == '\n' && spisor_sha256_file(layout->executable, actual) &&
                   strncmp(recorded + RUNTIME_SHA256_HEX_SIZE, actual, RUNTIME_SHA256_HEX_SIZE - 1) == 0;
    }
    free(bytes);
    return unpacked;
}

static cJSON *fetch_call(const acquisition *runtime) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || cJSON_AddStringToObject(call, "url", runtime->build.url) == NULL ||
        cJSON_AddStringToObject(call, "destination", runtime->staged.archive) == NULL ||
        cJSON_AddStringToObject(call, "sha256", runtime->build.sha256) == NULL ||
        cJSON_AddNumberToObject(call, "timeoutSeconds", DOWNLOAD_SECONDS) == NULL) {
        cJSON_Delete(call);
        return NULL;
    }
    return call;
}

/** The OS's own tar: System32's bsdtar on Windows, /usr/bin/tar or /bin/tar elsewhere; NULL when there is none. */
static char *system_tar(void) {
#if defined(_WIN32)
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    char narrow[4 * MAX_PATH];
    int written = length == 0 || length >= MAX_PATH
                      ? 0
                      : WideCharToMultiByte(CP_UTF8, 0, folder, (int)length, narrow, (int)sizeof narrow - 1, NULL, NULL);
    if (written <= 0) {
        return NULL;
    }
    narrow[written] = 0;
    const char *segments[2] = {narrow, "tar.exe"};
    char *tar = joined(segments, 2);
#else
    char *tar = runtime_catalog_copy(spisor_file_exists("/usr/bin/tar") ? "/usr/bin/tar" : "/bin/tar");
#endif
    if (tar != NULL && !spisor_file_exists(tar)) {
        free(tar);
        return NULL;
    }
    return tar;
}

static cJSON *tar_arguments(const acquisition *runtime) {
    cJSON *arguments = cJSON_CreateArray();
    if (arguments == NULL || !runtime_install_add_text(arguments, runtime->zip ? "-xf" : "-xzf") ||
        !runtime_install_add_text(arguments, runtime->staged.archive) || !runtime_install_add_text(arguments, "-C") ||
        !runtime_install_add_text(arguments, runtime->staged.directory) ||
        !runtime_install_add_text(arguments, runtime->member)) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/** Unpacks only the executable from the staged archive with the OS's tar; NULL on success, else the refusal. */
static cJSON *unpack(const acquisition *runtime) {
    char *tar = system_tar();
    if (tar == NULL) {
        return refused("this system has no tar to unpack ", runtime->staged.archive,
                       "install tar (bsdtar on Windows 10 1803 or later), then run install again");
    }
    cJSON *arguments = tar_arguments(runtime);
    int exit_code = -1;
    char *problem = NULL;
    int ran = arguments != NULL && runtime_run_to_exit(tar, arguments, TAR_MILLISECONDS, &exit_code, NULL, &problem);
    cJSON_Delete(arguments);
    free(tar);
    cJSON *refusal = NULL;
    if (!ran) {
        refusal = runtime_refusal(problem == NULL ? "the runtime archive could not be unpacked" : problem, AGAIN_FIX);
    } else if (exit_code != 0 || !spisor_file_exists(runtime->staged.executable)) {
        refusal = refused("the runtime archive holds no ", runtime->member, AGAIN_FIX);
    }
    free(problem);
    return refusal;
}

static int write_marker(const acquisition *runtime) {
    char executable_hash[RUNTIME_SHA256_HEX_SIZE];
    if (!spisor_sha256_file(runtime->staged.executable, executable_hash)) {
        return 0;
    }
    const char *parts[4] = {runtime->build.sha256, "\n", executable_hash, "\n"};
    char *text = runtime_cache_text(parts, 4);
    int written = text != NULL && spisor_replace_file(runtime->staged.marker, (const uint8_t *)text, strlen(text)) == 1;
    free(text);
    return written;
}

/** Fetches, unpacks and marks the runtime in the staging folder; NULL on success, else the refusal. */
static cJSON *build_staged(const acquisition *runtime) {
    if (!runtime_remove_tree(runtime->staged.directory) || spisor_make_directories(runtime->staged.directory) != 1) {
        return refused("the runtime's staging folder could not be made: ", runtime->staged.directory, CACHE_FIX);
    }
    cJSON *refusal = NULL;
    cJSON *stored = runtime_call_or_refuse(runtime_fetch_get, fetch_call(runtime), &refusal);
    cJSON_Delete(stored);
    if (stored == NULL) {
        return refusal != NULL ? refusal : runtime_refusal("the runtime archive could not be fetched", AGAIN_FIX);
    }
    refusal = unpack(runtime);
    runtime_remove_file_entry(runtime->staged.archive);
    if (refusal == NULL && !write_marker(runtime)) {
        refusal = refused("the runtime's marker could not be written: ", runtime->staged.marker, CACHE_FIX);
    }
    return refusal;
}

static int rename_directory(const char *from, const char *to) {
#if defined(_WIN32)
    return spisor_windows_move_directory(from, to);
#else
    return rename(from, to) == 0;
#endif
}

/**
 * Moves the staged runtime into its final folder; NULL on success, else the refusal.
 *
 * @implNote A rename onto a folder that exists fails, so of two installs acquiring at once the second
 * finds the first's intact runtime and accepts it; only a final folder that is not intact is removed
 * first. No lock is held (Ruling L): the staging folder is this process's alone.
 */
static cJSON *publish(const acquisition *runtime) {
    if (rename_directory(runtime->staged.directory, runtime->final.directory) || is_unpacked(runtime, &runtime->final)) {
        return NULL;
    }
    if (runtime_remove_tree(runtime->final.directory) &&
        (rename_directory(runtime->staged.directory, runtime->final.directory) || is_unpacked(runtime, &runtime->final))) {
        return NULL;
    }
    return refused("the runtime could not be moved into place at ", runtime->final.directory,
                   "close whatever runs the JavaScript runtime from the cache, then run install again");
}

/**
 * Removes every staging folder of platform under the version folder untouched for a day, which a crashed
 * acquisition left; a younger one may be another install's, still building.
 */
static void sweep_stale_staging(const acquisition *runtime, const char *platform) {
    const char *segments[2] = {runtime->versions, runtime->version};
    char *version_folder = joined(segments, 2);
    char prefix[96];
    snprintf(prefix, sizeof prefix, ".%s-staging-", platform);
    runtime_directory_listing listing;
    if (version_folder == NULL || !runtime_list_directory(version_folder, &listing)) {
        free(version_folder);
        return;
    }
    double now = runtime_now_milliseconds();
    for (size_t index = 0; index < listing.count; index++) {
        const char *name = listing.entries[index].name;
        const char *entry_segments[2] = {version_folder, name};
        char *path = strncmp(name, prefix, strlen(prefix)) == 0 ? joined(entry_segments, 2) : NULL;
        double newest = 0;
        if (path != NULL && runtime_newest_change(path, &newest) && now - newest > STALE_STAGING_MILLISECONDS) {
            runtime_remove_tree(path);
        }
        free(path);
    }
    runtime_free_directory_listing(&listing);
    free(version_folder);
}

/** @implNote A version still running holds its files, so one that cannot be removed waits for the next acquisition. */
static void remove_other_versions(const acquisition *runtime) {
    runtime_directory_listing listing;
    if (!runtime_list_directory(runtime->versions, &listing)) {
        return;
    }
    for (size_t index = 0; index < listing.count; index++) {
        const char *segments[2] = {runtime->versions, listing.entries[index].name};
        char *path = strcmp(listing.entries[index].name, runtime->version) == 0 ? NULL : joined(segments, 2);
        if (path != NULL) {
            runtime_remove_tree(path);
        }
        free(path);
    }
    runtime_free_directory_listing(&listing);
}

static cJSON *answer_of(const char *version, const char *executable, int fetched, const char *source) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddStringToObject(answer, "name", RUNTIME_NAME) == NULL ||
        cJSON_AddStringToObject(answer, "version", version) == NULL ||
        cJSON_AddStringToObject(answer, "executable", executable) == NULL ||
        cJSON_AddBoolToObject(answer, "fetched", fetched) == NULL || cJSON_AddStringToObject(answer, "source", source) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

static cJSON *acquire_pinned(const char *cache, const char *platform, cJSON **refusal) {
    acquisition runtime;
    memset(&runtime, 0, sizeof runtime);
    if (!build_for(platform, &runtime.build, &runtime.version)) {
        *refusal = refused(RUNTIME_NAME " " PINNED_VERSION " has no pinned build for ", platform, RUNTIME_FIX);
        return NULL;
    }
    runtime.zip = ends_with(runtime.build.url, ZIP_ENDING);
    if (!runtime.zip && !ends_with(runtime.build.url, TAR_GZ_ENDING)) {
        *refusal = refused("the pinned runtime is neither a .zip nor a .tar.gz: ", runtime.build.url, RUNTIME_FIX);
        return NULL;
    }
    cJSON *answer = NULL;
    if (lay_out(&runtime, cache, platform)) {
        sweep_stale_staging(&runtime, platform);
        int unpacked = is_unpacked(&runtime, &runtime.final);
        *refusal = unpacked ? NULL : build_staged(&runtime);
        if (!unpacked && *refusal == NULL) {
            before_publish(&runtime);
            *refusal = publish(&runtime);
        }
        runtime_remove_tree(runtime.staged.directory);
        answer = *refusal == NULL ? answer_of(runtime.version, runtime.final.executable, !unpacked, "acquired") : NULL;
        if (answer != NULL) {
            remove_other_versions(&runtime);
        }
    }
    discard(&runtime);
    return answer;
}

/** The leading number of a version line such as 22.1.0, or -1 when it has none. */
static long major_of(const char *version) {
    char *end = NULL;
    long major = strtol(version, &end, 10);
    return end == version || (*end != '.' && *end != 0) ? -1 : major;
}

static char *trimmed(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
                          text[length - 1] == '\n')) {
        length--;
    }
    return runtime_catalog_copy_bytes(text, length);
}

/** The version the node at executable reports, run once with a bounded wait; NULL when it reports none. */
static char *reported_version(const char *executable) {
    cJSON *arguments = cJSON_CreateArray();
    char *output = NULL;
    char *problem = NULL;
    int exit_code = -1;
    int ran = arguments != NULL && runtime_install_add_text(arguments, "-p") &&
              runtime_install_add_text(arguments, "process.versions.node") &&
              runtime_run_to_exit(executable, arguments, VERSION_MILLISECONDS, &exit_code, &output, &problem);
    char *version = ran && exit_code == 0 ? trimmed(output) : NULL;
    free(problem);
    free(output);
    cJSON_Delete(arguments);
    return version;
}

/**
 * A compatible node on the PATH of environment, as spec 5.3 orders it before acquiring one; NULL when
 * there is none, its version cannot be read, or its major is below COMPATIBLE_MAJOR.
 *
 * @implNote Found by detect's PATH rules and run once, only here at install, never by detection.
 */
static cJSON *runtime_on_path(const cJSON *environment) {
    char *executable = environment == NULL ? NULL : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, RUNTIME_NAME, environment);
    char *version = executable == NULL ? NULL : reported_version(executable);
    cJSON *answer = version != NULL && major_of(version) >= COMPATIBLE_MAJOR ? answer_of(version, executable, 0, "path")
                                                                            : NULL;
    free(version);
    free(executable);
    return answer;
}

/** Records the runtime answered as the one every host started on JavaScript runs with; 1 on success. */
static int record_runtime(const char *cache, const cJSON *answer) {
    const char *folder_segments[2] = {cache, "runtimes"};
    const char *file_segments[3] = {cache, "runtimes", RECORD_FILE};
    char *folder = joined(folder_segments, 2);
    char *file = joined(file_segments, 3);
    char *pretty = spisor_json_stringify_pretty(answer);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    int recorded = folder != NULL && file != NULL && text != NULL && spisor_make_directories(folder) == 1 &&
                   spisor_replace_file(file, (const uint8_t *)text, strlen(text)) == 1;
    free(text);
    free(pretty);
    free(file);
    free(folder);
    return recorded;
}

cJSON *runtime_acquire_javascript(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal) {
    int this_machine = strcmp(platform, runtime_platform_key()) == 0;
    cJSON *answer = this_machine ? runtime_on_path(environment) : NULL;
    if (answer == NULL) {
        answer = acquire_pinned(cache, platform, refusal);
    }
    if (answer != NULL && !record_runtime(cache, answer)) {
        cJSON_Delete(answer);
        answer = NULL;
        *refusal = refused("the JavaScript runtime could not be recorded under ", cache, CACHE_FIX);
    }
    return answer;
}

static cJSON *runtime_for(const cJSON *argument, cJSON **refusal) {
    const char *cache = runtime_install_text(argument, "cache");
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    const cJSON *environment = cJSON_GetObjectItemCaseSensitive(argument, "environment");
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache) || !cJSON_IsObject(environment) ||
        (platform != NULL && !(cJSON_IsString(platform) && runtime_is_platform_key(platform->valuestring)))) {
        *refusal = runtime_refusal("the argument carries no usable cache, environment or platform",
                                   "send {\"cache\": \"<absolute directory>\", \"environment\": {<name>: <value>}, "
                                   "\"platform\"?: \"<os>-<arch>\"}");
        return NULL;
    }
    return runtime_acquire_javascript(cache, platform == NULL ? runtime_platform_key() : platform->valuestring,
                                      environment, refusal);
}

RUNTIME_EXPORT int32_t runtime_install_runtime(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, runtime_for);
}
