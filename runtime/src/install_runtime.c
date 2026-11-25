#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "install_runtime.h"
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_text.h"
#include "directory_tree.h"
#include "prepared_undo.h"
#include "runtime/fetch.h"
#include "followed_entry.h"
#include "file_times.h"
#include "install_support.h"
#include "json_service.h"
#include "node_json_file.h"
#include "on_path.h"
#include "paths.h"
#include "platform_key.h"
#include "process_run.h"
#include "sha256_file.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>

#include "windows_file.h"
#define current_process_id() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define current_process_id() getpid()
#endif

#include "atomic.h"

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
#define RUNTIME_FIX                                                                                    \
    "install the ecosystem on win32-x64, linux-x64, darwin-x64 or darwin-arm64, or put Node 20 or newer " \
    "on the PATH"
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
    int out_of_memory;
} acquisition;

#ifdef SPISOR_INSTALL_TEST_HOOKS
static char *test_version = NULL;
static char *test_url = NULL;
static char *test_sha256 = NULL;
static runtime_build test_build = {NULL, NULL, NULL};

void runtime_install_use_runtime_build(const char *version, const char *url, const char *sha256) {
    spisor_memory_release(test_version);
    spisor_memory_release(test_url);
    spisor_memory_release(test_sha256);
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

/** Marks the acquisition out of memory: NULL, which with the mark is no success. */
static cJSON *ran_out(acquisition *runtime) {
    runtime->out_of_memory = 1;
    return NULL;
}

/** The refusal, or NULL with the acquisition marked out of memory when memory ran out building it. */
static cJSON *refusal_of(acquisition *runtime, cJSON *refusal) {
    return refusal != NULL ? refusal : ran_out(runtime);
}

static void discard_layout(runtime_layout *layout) {
    spisor_memory_release(layout->directory);
    spisor_memory_release(layout->archive);
    spisor_memory_release(layout->marker);
    spisor_memory_release(layout->executable);
}

static void discard(acquisition *runtime) {
    spisor_memory_release(runtime->versions);
    spisor_memory_release(runtime->stem);
    spisor_memory_release(runtime->member);
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
    static volatile long acquisitions = 0;
    return (unsigned long)runtime_atomic_increment(&acquisitions);
}

/**
 * @implNote Named by process and a number no other acquisition of this process takes, so two installs never build in
 * one folder.
 */
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

/** Whether the executable's hash is the one `recorded` names, marking the acquisition when memory runs out. */
static int is_recorded_executable(acquisition *runtime, const char *recorded, const char *executable) {
    char actual[NAULA_SHA256_HEX_SIZE];
    spisor_status hashed = spisor_sha256_file(executable, actual);
    if (hashed == SPISOR_OUT_OF_MEMORY) {
        runtime->out_of_memory = 1;
    }
    return hashed == SPISOR_OK && strncmp(recorded, actual, NAULA_SHA256_HEX_SIZE - 1) == 0;
}

/**
 * Whether layout's marker names the pinned archive and its executable still has the hash the marker names; 0 with the
 * acquisition marked out of memory when memory ran out reading either.
 */
static int is_unpacked(acquisition *runtime, const runtime_layout *layout) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(layout->marker, &length, &outcome);
    char recorded[2 * NAULA_SHA256_HEX_SIZE + 2];
    int unpacked = bytes != NULL && length == 2 * (NAULA_SHA256_HEX_SIZE - 1) + 2;
    if (outcome == SPISOR_READ_OUT_OF_MEMORY) {
        runtime->out_of_memory = 1;
    }
    if (unpacked) {
        memcpy(recorded, bytes, length);
        recorded[length] = 0;
        unpacked = strncmp(recorded, runtime->build.sha256, NAULA_SHA256_HEX_SIZE - 1) == 0 &&
                   recorded[NAULA_SHA256_HEX_SIZE - 1] == '\n' &&
                   is_recorded_executable(runtime, recorded + NAULA_SHA256_HEX_SIZE, layout->executable);
    }
    spisor_memory_release(bytes);
    return unpacked;
}

/** Fetches the runtime's archive: 1 once stored, else 0 with *refusal set or the acquisition out of memory. */
static int fetched_archive(acquisition *runtime, cJSON **refusal) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal fetch_refusal = SPISOR_INIT(spisor_refusal);
    argument.url = runtime->build.url;
    argument.destination = runtime->staged.archive;
    argument.sha256 = runtime->build.sha256;
    argument.timeout_seconds = DOWNLOAD_SECONDS;
    spisor_status status = fetch->get(fetch->context, &argument, &answer, &fetch_refusal);
    *refusal = NULL;
    if (status == SPISOR_OUT_OF_MEMORY) {
        ran_out(runtime);
    } else if (status != SPISOR_OK) {
        *refusal = refusal_of(runtime, fetch_refusal.detail != NULL
                                           ? runtime_refusal_of(&fetch_refusal)
                                           : runtime_refusal("the runtime archive could not be fetched", AGAIN_FIX));
    }
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &fetch_refusal);
    return status == SPISOR_OK;
}

/**
 * The OS's own tar: System32's bsdtar on Windows, /usr/bin/tar or /bin/tar elsewhere; NULL when there is none, or
 * with *out_of_memory set when memory ran out finding it.
 */
static char *system_tar(int *out_of_memory) {
#if defined(_WIN32)
    wchar_t folder[MAX_PATH];
    UINT length = GetSystemDirectoryW(folder, MAX_PATH);
    char narrow[4 * MAX_PATH];
    int written = length == 0 || length >= MAX_PATH
                      ? 0
                      : WideCharToMultiByte(CP_UTF8, 0, folder, (int)length, narrow, (int)sizeof narrow - 1, NULL,
                                            NULL);
    if (written <= 0) {
        return NULL;
    }
    narrow[written] = 0;
    const char *segments[2] = {narrow, "tar.exe"};
    char *tar = joined(segments, 2);
#else
    int has_usr_tar = spisor_probe_file("/usr/bin/tar") == SPISOR_FILE_PRESENT;
    char *tar = runtime_catalog_copy(has_usr_tar ? "/usr/bin/tar" : "/bin/tar");
#endif
    spisor_file_presence presence = tar == NULL ? SPISOR_FILE_OUT_OF_MEMORY : spisor_probe_file(tar);
    *out_of_memory = presence == SPISOR_FILE_OUT_OF_MEMORY;
    if (presence != SPISOR_FILE_PRESENT) {
        spisor_memory_release(tar);
        return NULL;
    }
    return tar;
}

static cJSON *tar_arguments(const acquisition *runtime) {
    cJSON *arguments = cJSON_CreateArray();
    if (arguments == NULL || !runtime_install_add_text(arguments, runtime->zip ? "-xOf" : "-xzOf") ||
        !runtime_install_add_text(arguments, "-") || !runtime_install_add_text(arguments, runtime->member)) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/** Makes the executable just unpacked runnable by its owner, as the archive's own mode would. */
static int mark_runnable(const char *executable) {
#if defined(_WIN32)
    (void)executable;
    return 1;
#else
    return chmod(executable, 0755) == 0;
#endif
}

static spisor_status make_executable_folder(const acquisition *runtime) {
    const char *segments[2] = {runtime->staged.executable, ".."};
    char *folder = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_status made = folder == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(folder);
    spisor_memory_release(folder);
    return made;
}

/** What an unpack that ran, or did not, left: NULL when the executable is runnable, else the refusal. */
static cJSON *unpacked_refusal(acquisition *runtime, spisor_status folder, int ran, const char *problem,
                               int exit_code) {
    if (folder == SPISOR_OUT_OF_MEMORY || (folder == SPISOR_OK && !ran && problem == NULL)) {
        return ran_out(runtime);
    }
    if (!ran) {
        return refusal_of(runtime, runtime_refusal(problem == NULL ? "the runtime archive could not be unpacked"
                                                                   : problem,
                                                   AGAIN_FIX));
    }
    spisor_file_presence presence = spisor_probe_file(runtime->staged.executable);
    if (presence == SPISOR_FILE_OUT_OF_MEMORY) {
        return ran_out(runtime);
    }
    if (exit_code != 0 || presence != SPISOR_FILE_PRESENT) {
        return refusal_of(runtime, refused("the runtime archive holds no ", runtime->member, AGAIN_FIX));
    }
    if (!mark_runnable(runtime->staged.executable)) {
        return refusal_of(runtime,
                          refused("the runtime could not be made runnable: ", runtime->staged.executable, CACHE_FIX));
    }
    return NULL;
}

/**
 * Unpacks only the executable from the staged archive with the OS's tar; NULL on success, else the refusal.
 *
 * @implNote tar reads the archive on its standard input and writes the executable on its standard output,
 * both opened here, so no cache path is ever an argument of tar (see runtime_run_between_files).
 */
static cJSON *unpack(acquisition *runtime) {
    int out_of_memory = 0;
    char *tar = system_tar(&out_of_memory);
    if (tar == NULL) {
        return out_of_memory ? ran_out(runtime)
                             : refusal_of(runtime, refused("this system has no tar to unpack ", runtime->staged.archive,
                                                           "install tar (bsdtar on Windows 10 1803 or later), then "
                                                           "run install again"));
    }
    cJSON *arguments = tar_arguments(runtime);
    int exit_code = -1;
    char *problem = NULL;
    spisor_status folder = arguments == NULL ? SPISOR_OUT_OF_MEMORY : make_executable_folder(runtime);
    int ran = folder == SPISOR_OK && runtime_run_between_files(tar, arguments, TAR_MILLISECONDS,
                                                               runtime->staged.archive, runtime->staged.executable,
                                                               &exit_code, &problem);
    cJSON_Delete(arguments);
    spisor_memory_release(tar);
    cJSON *refusal = unpacked_refusal(runtime, folder, ran, problem, exit_code);
    spisor_memory_release(problem);
    return refusal;
}

/** Writes the marker naming the archive and the executable's hash: NULL once written, else the refusal. */
static cJSON *marker_refusal(acquisition *runtime) {
    char executable_hash[NAULA_SHA256_HEX_SIZE];
    char *text = NULL;
    spisor_status written = spisor_sha256_file(runtime->staged.executable, executable_hash);
    if (written == SPISOR_OK) {
        const char *parts[4] = {runtime->build.sha256, "\n", executable_hash, "\n"};
        text = runtime_cache_text(parts, 4);
        written = text == NULL ? SPISOR_OUT_OF_MEMORY
                               : spisor_replace_file(runtime->staged.marker, (const uint8_t *)text, strlen(text));
    }
    spisor_memory_release(text);
    if (written == SPISOR_OUT_OF_MEMORY) {
        return ran_out(runtime);
    }
    return written == SPISOR_OK
               ? NULL
               : refusal_of(runtime, refused("the runtime's marker could not be written: ", runtime->staged.marker,
                                             CACHE_FIX));
}

/** Fetches, unpacks and marks the runtime in the staging folder; NULL on success, else the refusal. */
static cJSON *build_staged(acquisition *runtime) {
    spisor_status made = runtime_remove_tree(runtime->staged.directory);
    if (made == SPISOR_OK) {
        made = spisor_make_directories(runtime->staged.directory);
    }
    if (made != SPISOR_OK) {
        return made == SPISOR_OUT_OF_MEMORY
                   ? ran_out(runtime)
                   : refusal_of(runtime, refused("the runtime's staging folder could not be made: ",
                                                 runtime->staged.directory, CACHE_FIX));
    }
    cJSON *refusal = NULL;
    if (!fetched_archive(runtime, &refusal)) {
        return refusal;
    }
    refusal = unpack(runtime);
    spisor_remove_file_in_place(runtime->staged.archive);
    return refusal != NULL || runtime->out_of_memory ? refusal : marker_refusal(runtime);
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
 * first. No lock is held: the staging folder is this process's alone.
 */
static cJSON *publish(acquisition *runtime) {
    if (rename_directory(runtime->staged.directory, runtime->final.directory) ||
        is_unpacked(runtime, &runtime->final) || runtime->out_of_memory) {
        return NULL;
    }
    if (runtime_remove_tree(runtime->final.directory) == SPISOR_OK &&
        (rename_directory(runtime->staged.directory, runtime->final.directory) ||
         is_unpacked(runtime, &runtime->final) || runtime->out_of_memory)) {
        return NULL;
    }
    return refusal_of(runtime, refused("the runtime could not be moved into place at ", runtime->final.directory,
                                       "close whatever runs the JavaScript runtime from the cache, then run install "
                                       "again"));
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
    if (version_folder == NULL || runtime_list_directory(version_folder, &listing) != SPISOR_OK) {
        spisor_memory_release(version_folder);
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
        spisor_memory_release(path);
    }
    runtime_free_directory_listing(&listing);
    spisor_memory_release(version_folder);
}

/** @implNote A version still running holds its files, so one that cannot be removed waits for the next acquisition. */
static void remove_other_versions(const acquisition *runtime) {
    runtime_directory_listing listing;
    if (runtime_list_directory(runtime->versions, &listing) != SPISOR_OK) {
        return;
    }
    for (size_t index = 0; index < listing.count; index++) {
        const char *segments[2] = {runtime->versions, listing.entries[index].name};
        char *path = strcmp(listing.entries[index].name, runtime->version) == 0 ? NULL : joined(segments, 2);
        if (path != NULL) {
            runtime_remove_tree(path);
        }
        spisor_memory_release(path);
    }
    runtime_free_directory_listing(&listing);
}

static cJSON *answer_of(const char *version, const char *executable, int fetched, const char *source) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddStringToObject(answer, "name", RUNTIME_NAME) == NULL ||
        cJSON_AddStringToObject(answer, "version", version) == NULL ||
        cJSON_AddStringToObject(answer, "executable", executable) == NULL ||
        cJSON_AddBoolToObject(answer, "fetched", fetched) == NULL ||
        cJSON_AddStringToObject(answer, "source", source) == NULL) {
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
        *refusal = unpacked || runtime.out_of_memory ? NULL : build_staged(&runtime);
        if (!unpacked && *refusal == NULL && !runtime.out_of_memory) {
            before_publish(&runtime);
            *refusal = publish(&runtime);
        }
        runtime_remove_tree(runtime.staged.directory);
        int acquired = *refusal == NULL && !runtime.out_of_memory;
        answer = acquired ? answer_of(runtime.version, runtime.final.executable, !unpacked, "acquired") : NULL;
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

/**
 * The version the node at executable reports, run once with a bounded wait; NULL when it reports none, or, with
 * *out_of_memory set, when memory ran out.
 */
static char *reported_version(const char *executable, int *out_of_memory) {
    cJSON *arguments = cJSON_CreateArray();
    char *output = NULL;
    char *problem = NULL;
    int exit_code = -1;
    int built = arguments != NULL && runtime_install_add_text(arguments, "-p") &&
                runtime_install_add_text(arguments, "process.versions.node");
    int ran = built && runtime_run_to_exit(executable, arguments, VERSION_MILLISECONDS, &exit_code, &output, &problem);
    char *version = ran && exit_code == 0 ? trimmed(output) : NULL;
    *out_of_memory = !built || (!ran && problem == NULL) || (ran && exit_code == 0 && version == NULL);
    spisor_memory_release(problem);
    spisor_memory_release(output);
    cJSON_Delete(arguments);
    return version;
}

/**
 * A compatible node on the PATH of environment, as spec 5.3 orders it before acquiring one; NULL when
 * there is none, its version cannot be read, or its major is below COMPATIBLE_MAJOR, and NULL with
 * *out_of_memory set when memory ran out finding or probing it, so a missing node is never assumed.
 *
 * @implNote Found by detect's PATH rules and run once, only here at install, never by detection.
 */
static cJSON *runtime_on_path(const cJSON *environment, int *out_of_memory) {
    char *executable = NULL;
    runtime_on_path_status found = environment == NULL ? RUNTIME_ON_PATH_ABSENT
                                                       : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, RUNTIME_NAME,
                                                                              environment, &executable);
    *out_of_memory = found == RUNTIME_ON_PATH_OUT_OF_MEMORY;
    char *version = executable == NULL ? NULL : reported_version(executable, out_of_memory);
    int compatible = version != NULL && major_of(version) >= COMPATIBLE_MAJOR;
    cJSON *answer = compatible ? answer_of(version, executable, 0, "path") : NULL;
    *out_of_memory = *out_of_memory || (compatible && answer == NULL);
    spisor_memory_release(version);
    spisor_memory_release(executable);
    return answer;
}

/**
 * Records the runtime answered as the one every host started on JavaScript runs with; 1 on success, and 0 with
 * *out_of_memory set when memory ran out.
 */
static int record_runtime(const char *cache, const cJSON *answer, int *out_of_memory) {
    const char *folder_segments[2] = {cache, "runtimes"};
    const char *file_segments[3] = {cache, "runtimes", RECORD_FILE};
    char *folder = joined(folder_segments, 2);
    char *file = joined(file_segments, 3);
    char *pretty = spisor_json_stringify_pretty(answer);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    spisor_status recorded = folder == NULL || file == NULL || text == NULL ? SPISOR_OUT_OF_MEMORY
                                                                             : spisor_make_directories(folder);
    if (recorded == SPISOR_OK) {
        recorded = spisor_replace_file(file, (const uint8_t *)text, strlen(text));
    }
    *out_of_memory = recorded == SPISOR_OUT_OF_MEMORY;
    spisor_memory_release(text);
    spisor_memory_release(pretty);
    spisor_memory_release(file);
    spisor_memory_release(folder);
    return recorded == SPISOR_OK;
}

cJSON *runtime_acquire_javascript(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal) {
    int this_machine = strcmp(platform, runtime_platform_key()) == 0;
    int out_of_memory = 0;
    cJSON *answer = this_machine ? runtime_on_path(environment, &out_of_memory) : NULL;
    if (out_of_memory) {
        return NULL;
    }
    if (answer == NULL) {
        answer = acquire_pinned(cache, platform, refusal);
    }
    if (answer != NULL && !record_runtime(cache, answer, &out_of_memory)) {
        cJSON_Delete(answer);
        answer = NULL;
        *refusal = out_of_memory ? NULL
                                 : refused("the JavaScript runtime could not be recorded under ", cache, CACHE_FIX);
    }
    return answer;
}

/** The executable recorded under cache, NULL when none is, and NULL with *out_of_memory set when memory runs out. */
static char *recorded_executable(const char *cache, int *out_of_memory) {
    const char *segments[3] = {cache, "runtimes", RECORD_FILE};
    char *record = joined(segments, 3);
    *out_of_memory = record == NULL;
    cJSON *recorded = record == NULL ? NULL : runtime_read_node_json(record, out_of_memory);
    const char *executable = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(recorded, "executable"));
    int out_of_memory_asking = 0;
    int followed = executable != NULL && runtime_is_followed_file(executable, 0, &out_of_memory_asking);
    char *copy = followed ? runtime_catalog_copy(executable) : NULL;
    *out_of_memory = *out_of_memory || out_of_memory_asking || (followed && copy == NULL);
    cJSON_Delete(recorded);
    spisor_memory_release(record);
    return copy;
}

char *runtime_javascript_executable(const char *cache, const cJSON *environment, cJSON **refusal) {
    int out_of_memory;
    char *executable = recorded_executable(cache, &out_of_memory);
    if (executable != NULL || out_of_memory) {
        return executable;
    }
    cJSON *acquired = runtime_acquire_javascript(cache, runtime_platform_key(), environment, refusal);
    const char *found = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(acquired, "executable"));
    executable = found == NULL ? NULL : runtime_catalog_copy(found);
    if (acquired != NULL && found == NULL) {
        *refusal = refused("the JavaScript runtime acquired again could not be read back under ", cache, CACHE_FIX);
    }
    cJSON_Delete(acquired);
    return executable;
}

cJSON *runtime_install_runtime(const char *cache, const char *platform, const cJSON *environment, cJSON **refusal) {
    *refusal = NULL;
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache) || !cJSON_IsObject(environment) ||
        (platform != NULL && !runtime_is_platform_key(platform))) {
        *refusal = runtime_refusal("the call carries no usable cache, environment or platform",
                                   "pass the cache as an absolute directory, the environment as {<name>: <value>} "
                                   "and the platform, when given, as <os>-<arch>");
        return NULL;
    }
    return runtime_acquire_javascript(cache, platform == NULL ? runtime_platform_key() : platform, environment,
                                      refusal);
}
