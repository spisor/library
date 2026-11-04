#include "install_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "catalog_text.h"
#include "directory_tree.h"
#include "fetch.h"
#include "install_support.h"
#include "json_service.h"
#include "paths.h"
#include "platform_key.h"
#include "process_run.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#ifdef SPISOR_INSTALL_TEST_HOOKS
#include "install_hooks.h"
#endif

#define RUNTIME_NAME "node"
#define PINNED_VERSION "26.13.0"
#define UNPACKED_MARKER ".unpacked"
#define DOWNLOAD_SECONDS 1800
#define RUNTIME_FIX "install the ecosystem on win32-x64, linux-x64, darwin-x64 or darwin-arm64, or use a host that needs no JavaScript runtime"
#define AGAIN_FIX "run install again; if it repeats, report the pinned archive"
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

/** One acquisition: the build chosen and every path it lays out. */
typedef struct acquisition {
    const char *version;
    runtime_build build;
    int zip;
    char *versions;
    char *directory;
    char *archive;
    char *stem;
    char *member;
    char *executable;
    char *marker;
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

static void discard(acquisition *runtime) {
    free(runtime->versions);
    free(runtime->directory);
    free(runtime->archive);
    free(runtime->stem);
    free(runtime->member);
    free(runtime->executable);
    free(runtime->marker);
}

/** Lays out the paths of the build's archive, named by its url's last segment. 1 on success. */
static int lay_out(acquisition *runtime, const char *cache, const char *platform) {
    const char *archive_name = strrchr(runtime->build.url, '/');
    archive_name = archive_name == NULL ? runtime->build.url : archive_name + 1;
    const char *ending = runtime->zip ? ZIP_ENDING : TAR_GZ_ENDING;
    runtime->stem = runtime_catalog_copy_bytes(archive_name, strlen(archive_name) - strlen(ending));
    const char *versions_segments[3] = {cache, "runtimes", RUNTIME_NAME};
    const char *directory_segments[5] = {cache, "runtimes", RUNTIME_NAME, runtime->version, platform};
    runtime->versions = joined(versions_segments, 3);
    runtime->directory = joined(directory_segments, 5);
    const char *archive_segments[2] = {runtime->directory, archive_name};
    const char *marker_segments[2] = {runtime->directory, UNPACKED_MARKER};
    runtime->archive = runtime->directory == NULL ? NULL : joined(archive_segments, 2);
    runtime->marker = runtime->directory == NULL ? NULL : joined(marker_segments, 2);
    const char *member_parts[2] = {runtime->stem, runtime->zip ? "/node.exe" : "/bin/node"};
    runtime->member = runtime->stem == NULL ? NULL : runtime_cache_text(member_parts, 2);
    const char *executable_segments[3] = {runtime->directory, runtime->stem, runtime->zip ? "node.exe" : "bin/node"};
    runtime->executable = runtime->directory == NULL || runtime->stem == NULL ? NULL : joined(executable_segments, 3);
    return runtime->versions != NULL && runtime->archive != NULL && runtime->marker != NULL && runtime->member != NULL &&
           runtime->executable != NULL;
}

/** Whether the marker names the pinned archive and the executable still has the hash it names. */
static int is_unpacked(const acquisition *runtime) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(runtime->marker, &length);
    char recorded[2 * RUNTIME_SHA256_HEX_SIZE + 2];
    char actual[RUNTIME_SHA256_HEX_SIZE];
    int unpacked = bytes != NULL && length == 2 * (RUNTIME_SHA256_HEX_SIZE - 1) + 2;
    if (unpacked) {
        memcpy(recorded, bytes, length);
        recorded[length] = 0;
        unpacked = strncmp(recorded, runtime->build.sha256, RUNTIME_SHA256_HEX_SIZE - 1) == 0 &&
                   recorded[RUNTIME_SHA256_HEX_SIZE - 1] == '\n' && spisor_sha256_file(runtime->executable, actual) &&
                   strncmp(recorded + RUNTIME_SHA256_HEX_SIZE, actual, RUNTIME_SHA256_HEX_SIZE - 1) == 0;
    }
    free(bytes);
    return unpacked;
}

static cJSON *refused(const char *message, const char *subject, const char *fix) {
    const char *parts[2] = {message, subject};
    return runtime_cache_refusal(parts, 2, fix);
}

static cJSON *fetch_call(const acquisition *runtime) {
    cJSON *call = cJSON_CreateObject();
    if (call == NULL || cJSON_AddStringToObject(call, "url", runtime->build.url) == NULL ||
        cJSON_AddStringToObject(call, "destination", runtime->archive) == NULL ||
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
        !runtime_install_add_text(arguments, runtime->archive) || !runtime_install_add_text(arguments, "-C") ||
        !runtime_install_add_text(arguments, runtime->directory) || !runtime_install_add_text(arguments, runtime->member)) {
        cJSON_Delete(arguments);
        return NULL;
    }
    return arguments;
}

/** Unpacks only the executable from the archive with the OS's tar; NULL on success, else the refusal. */
static cJSON *unpack(const acquisition *runtime) {
    char *tar = system_tar();
    if (tar == NULL) {
        return refused("this system has no tar to unpack ", runtime->archive,
                       "install tar (bsdtar on Windows 10 1803 or later), then run install again");
    }
    cJSON *arguments = tar_arguments(runtime);
    int exit_code = -1;
    char *problem = NULL;
    int ran = arguments != NULL && runtime_run_to_exit(tar, arguments, &exit_code, &problem);
    cJSON_Delete(arguments);
    free(tar);
    cJSON *refusal = NULL;
    if (!ran) {
        refusal = problem == NULL ? NULL : runtime_refusal(problem, AGAIN_FIX);
    } else if (exit_code != 0 || !spisor_file_exists(runtime->executable)) {
        refusal = refused("the runtime archive holds no ", runtime->member, AGAIN_FIX);
    }
    free(problem);
    return refusal;
}

static int write_marker(const acquisition *runtime) {
    char executable_hash[RUNTIME_SHA256_HEX_SIZE];
    if (!spisor_sha256_file(runtime->executable, executable_hash)) {
        return 0;
    }
    const char *parts[4] = {runtime->build.sha256, "\n", executable_hash, "\n"};
    char *text = runtime_cache_text(parts, 4);
    int written = text != NULL && spisor_replace_file(runtime->marker, (const uint8_t *)text, strlen(text)) == 1;
    free(text);
    return written;
}

/** Fetches the archive, keeps only its executable and records both hashes; NULL on success, else the refusal. */
static cJSON *unpack_fresh(const acquisition *runtime) {
    const char *stem_segments[2] = {runtime->directory, runtime->stem};
    char *stem_directory = joined(stem_segments, 2);
    int cleared = stem_directory != NULL && runtime_remove_file_entry(runtime->marker) == RUNTIME_ENTRY_DONE &&
                  runtime_remove_tree(stem_directory) && spisor_make_directories(runtime->directory) == 1;
    free(stem_directory);
    if (!cleared) {
        return refused("the runtime folder could not be cleared: ", runtime->directory,
                       "check that SPISOR_CACHE names a writable folder, then run install again");
    }
    cJSON *refusal = NULL;
    cJSON *stored = runtime_call_or_refuse(runtime_fetch_get, fetch_call(runtime), &refusal);
    cJSON_Delete(stored);
    if (stored == NULL) {
        return refusal;
    }
    refusal = unpack(runtime);
    runtime_remove_file_entry(runtime->archive);
    if (refusal == NULL && !write_marker(runtime)) {
        refusal = refused("the runtime's marker could not be written: ", runtime->marker,
                          "check that SPISOR_CACHE names a writable folder, then run install again");
    }
    return refusal;
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

static cJSON *answer_of(const acquisition *runtime, int fetched) {
    cJSON *answer = cJSON_CreateObject();
    if (answer == NULL || cJSON_AddStringToObject(answer, "name", RUNTIME_NAME) == NULL ||
        cJSON_AddStringToObject(answer, "version", runtime->version) == NULL ||
        cJSON_AddStringToObject(answer, "executable", runtime->executable) == NULL ||
        cJSON_AddBoolToObject(answer, "fetched", fetched) == NULL) {
        cJSON_Delete(answer);
        return NULL;
    }
    return answer;
}

cJSON *runtime_acquire_javascript(const char *cache, const char *platform, cJSON **refusal) {
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
        int unpacked = is_unpacked(&runtime);
        *refusal = unpacked ? NULL : unpack_fresh(&runtime);
        answer = *refusal == NULL ? answer_of(&runtime, !unpacked) : NULL;
        if (answer != NULL) {
            remove_other_versions(&runtime);
        }
    }
    discard(&runtime);
    return answer;
}

static cJSON *runtime_for(const cJSON *argument, cJSON **refusal) {
    const char *cache = runtime_install_text(argument, "cache");
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(argument, "platform");
    if (cache == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, cache) ||
        (platform != NULL && !(cJSON_IsString(platform) && runtime_is_platform_key(platform->valuestring)))) {
        *refusal = runtime_refusal("the argument carries no usable cache or platform",
                                   "send {\"cache\": \"<absolute directory>\", \"platform\"?: \"<os>-<arch>\"}");
        return NULL;
    }
    return runtime_acquire_javascript(cache, platform == NULL ? runtime_platform_key() : platform->valuestring, refusal);
}

RUNTIME_EXPORT int32_t runtime_install_runtime(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    return runtime_serve_json(in, in_len, out, out_len, runtime_for);
}
