#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "allocation.h"
#include "catalog_semver.h"
#include "directory_tree.h"
#include "node_json_file.h"
#include "on_path.h"
#include "paths.h"
#include "platform_key.h"
#include "process_environment.h"
#include "process_run.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include "host_arguments.h"
#define EXECUTABLE_ENDING ".exe"
#define SHARED_LIBRARY_PREFIX ""
#define SHARED_LIBRARY_ENDING ".dll"
#else
#include <sys/stat.h>
#define EXECUTABLE_ENDING ""
#define SHARED_LIBRARY_PREFIX "lib"
#if defined(__APPLE__)
#define SHARED_LIBRARY_ENDING ".dylib"
#else
#define SHARED_LIBRARY_ENDING ".so"
#endif
#endif

#define USAGE                                                                                                         \
    "usage: stage-bundle host <directory> <version> <spisor-host> <spisor-boot> [<runtime-launcher>]\n"                \
    "       stage-bundle library <directory> <library.json> <library binary>\n"                                       \
    "       stage-bundle javascript-host <directory> <version> <weivel/typescript checkout> <node pin> "               \
    "<weivel_core> <weivel_standard>\n"
#define BUNDLER_MILLISECONDS 300000u
#define ENGINE_BOUND ">="
#define KOFFI_FOLDER "node_modules/koffi"

/** The files of koffi's package the bundled program loads, beside the one prebuilt of this platform. */
static const char *const KOFFI_FILES[] = {"package.json", "index.js", "LICENSE.txt"};

/** What javascript-host's bundle is staged from: the version, the checkout's minimum Node and the read pin. */
typedef struct javascript_host_inputs {
    const char *directory;
    const char *version;
    const char *checkout;
    cJSON *pin;
    char minimum[64];
} javascript_host_inputs;

static char *joined(const char *base, const char *rest) {
    const char *segments[2] = {base, rest};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

static const char *base_name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash > backslash ? slash : backslash;
    return last == NULL ? path : last + 1;
}

/** Copies source to name inside directory, name a relative path whose folders are made first. */
static int copy_into(const char *source, const char *directory, const char *name) {
    char *destination = joined(directory, name);
    char *parent = destination == NULL ? NULL : joined(destination, "..");
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(source, &length, &outcome);
    int copied = parent != NULL && bytes != NULL && spisor_make_directories(parent) == SPISOR_OK &&
                 spisor_replace_file(destination, bytes, length) == SPISOR_OK;
    spisor_memory_release(parent);
#if !defined(_WIN32)
    copied = copied && chmod(destination, 0755) == 0;
#endif
    if (outcome == SPISOR_READ_OUT_OF_MEMORY) {
        fprintf(stderr, "stage-bundle: memory ran out reading %s\n", source);
    } else if (!copied) {
        fprintf(stderr, "stage-bundle: %s could not be copied to %s\n", source, destination == NULL ? name : destination);
    }
    spisor_memory_release(bytes);
    spisor_memory_release(destination);
    return copied;
}

static int start_empty(const char *directory) {
    if (runtime_remove_tree(directory) != SPISOR_OK || spisor_make_directories(directory) != SPISOR_OK) {
        fprintf(stderr, "stage-bundle: %s could not be emptied\n", directory);
        return 0;
    }
    return 1;
}

/** host.json as catalog build reads a native host: its id, version, runtime and this platform's executable as entry. */
static int write_host_manifest(const char *directory, const char *version) {
    char entry[256];
    snprintf(entry, sizeof entry, "%s/spisor-host" EXECUTABLE_ENDING, runtime_platform_key());
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "id", "spisor-host");
    cJSON_AddStringToObject(manifest, "version", version);
    cJSON_AddStringToObject(manifest, "runtime", "native");
    cJSON_AddStringToObject(manifest, "entry", entry);
    char *text = spisor_json_stringify_pretty(manifest);
    char *path = joined(directory, "host.json");
    int written = text != NULL && path != NULL &&
        spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK;
    spisor_memory_release(path);
    spisor_memory_release(text);
    cJSON_Delete(manifest);
    return written;
}

/** spisor-host's bundle: host.json and <platform>/ holding spisor-host, spisor-boot and, on Windows, runtime-launcher.exe. */
static int stage_host(int count, char **arguments) {
    if (count < 6 || count > 7) {
        fputs(USAGE, stderr);
        return 2;
    }
    const char *directory = arguments[2];
    char *platform = joined(directory, runtime_platform_key());
    int staged = platform != NULL && start_empty(directory) && spisor_make_directories(platform) == SPISOR_OK &&
                 write_host_manifest(directory, arguments[3]) &&
                 copy_into(arguments[4], platform, "spisor-host" EXECUTABLE_ENDING) &&
                 copy_into(arguments[5], platform, "spisor-boot" EXECUTABLE_ENDING) &&
                 (count == 6 || copy_into(arguments[6], platform, "runtime-launcher.exe"));
    spisor_memory_release(platform);
    return staged ? 0 : 1;
}

/** The runtime library's directory as catalog build reads one: its library.json beside its binary. */
static int stage_library(int count, char **arguments) {
    if (count != 5) {
        fputs(USAGE, stderr);
        return 2;
    }
    const char *directory = arguments[2];
    int staged = start_empty(directory) && copy_into(arguments[3], directory, "library.json") &&
                 copy_into(arguments[4], directory, base_name_of(arguments[4]));
    return staged ? 0 : 1;
}

static int ends_with(const char *text, const char *ending) {
    size_t length = strlen(text);
    size_t ending_length = strlen(ending);
    return length >= ending_length && strcmp(text + length - ending_length, ending) == 0;
}

/** Whether text is a SHA-256 as nodejs.org's SHASUMS256.txt prints one: 64 lowercase hexadecimal digits. */
static int is_sha256(const char *text) {
    size_t length = text == NULL ? 0 : strlen(text);
    for (size_t index = 0; index < length; index++) {
        if (!((text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f'))) {
            return 0;
        }
    }
    return length == 64;
}

/** Whether build is {url, sha256}, the url an https one ending .zip or .tar.gz, printing what is wrong when not. */
static int is_build(const char *path, const cJSON *build) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(build, "url"));
    const char *sha256 = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(build, "sha256"));
    int fetchable = url != NULL && strncmp(url, "https://", strlen("https://")) == 0 &&
                    (ends_with(url, ".zip") || ends_with(url, ".tar.gz"));
    if (!fetchable) {
        fprintf(stderr, "stage-bundle: %s pins %s for %s, not an https url ending .zip or .tar.gz\n", path,
                url == NULL ? "no url" : url, build->string);
    } else if (!is_sha256(sha256)) {
        fprintf(stderr, "stage-bundle: %s pins %s for %s, not a sha256 of 64 lowercase hexadecimal digits\n", path,
                sha256 == NULL ? "no sha256" : sha256, build->string);
    }
    return fetchable && is_sha256(sha256);
}

/** Whether every build in builds is one, and builds holds this platform's, printing what is wrong when not. */
static int are_builds(const char *path, const cJSON *builds) {
    const cJSON *build;
    cJSON_ArrayForEach(build, builds) {
        if (!is_build(path, build)) {
            return 0;
        }
    }
    if (cJSON_GetObjectItemCaseSensitive(builds, runtime_platform_key()) == NULL) {
        fprintf(stderr, "stage-bundle: %s pins no build for %s, the platform this bundle is staged for\n", path,
                runtime_platform_key());
        return 0;
    }
    return 1;
}

static cJSON *read_json(const char *path) {
    int out_of_memory = 0;
    cJSON *read = runtime_read_node_json(path, &out_of_memory);
    if (read == NULL) {
        fprintf(stderr, "stage-bundle: %s %s\n", path, out_of_memory ? "ran out of memory reading" : "is no JSON file");
    }
    return read;
}

/** The lowest Node the checkout's package.json engines.node names as >=<version>, into inputs->minimum. */
static int read_minimum(javascript_host_inputs *inputs) {
    char *path = joined(inputs->checkout, "package.json");
    cJSON *package = path == NULL ? NULL : read_json(path);
    const char *engine =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(package, "engines"), "node"));
    size_t bound = strlen(ENGINE_BOUND);
    int read = engine != NULL && strncmp(engine, ENGINE_BOUND, bound) == 0 &&
               runtime_catalog_is_plain_version(engine + bound) && strlen(engine + bound) < sizeof inputs->minimum;
    if (read) {
        snprintf(inputs->minimum, sizeof inputs->minimum, "%s", engine + bound);
    } else if (package != NULL) {
        fprintf(stderr, "stage-bundle: %s names engines.node %s, not >=<major>.<minor>.<patch>\n", path,
                engine == NULL ? "(none)" : engine);
    }
    cJSON_Delete(package);
    spisor_memory_release(path);
    return read;
}

/**
 * The pin file as {pinned, builds}, refused unless pinned is a plain version no older than the checkout's minimum and
 * every build is one stage-bundle can vouch for, this platform's among them.
 */
static int read_pin(javascript_host_inputs *inputs, const char *path) {
    inputs->pin = read_json(path);
    const char *pinned = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inputs->pin, "pinned"));
    const cJSON *builds = cJSON_GetObjectItemCaseSensitive(inputs->pin, "builds");
    int shaped = runtime_catalog_is_plain_version(pinned) && cJSON_IsObject(builds);
    if (inputs->pin != NULL && !shaped) {
        fprintf(stderr, "stage-bundle: %s is not {\"pinned\": \"<major>.<minor>.<patch>\", \"builds\": {...}}\n", path);
        return 0;
    }
    if (shaped && runtime_catalog_compare_versions(pinned, inputs->minimum) < 0) {
        fprintf(stderr, "stage-bundle: %s pins Node %s, older than the %s the program needs\n", path, pinned,
                inputs->minimum);
        return 0;
    }
    return shaped && are_builds(path, builds);
}

/** Runs the checkout's bundler on the PATH's node, writing the program's two files into the directory. */
static int run_bundler(const javascript_host_inputs *inputs) {
    cJSON *environment = runtime_process_environment();
    char *node = NULL;
    runtime_on_path_status found = environment == NULL ? RUNTIME_ON_PATH_OUT_OF_MEMORY
                                                       : runtime_find_on_path(RUNTIME_HOST_IS_WINDOWS, "node",
                                                                              environment, &node);
    char *bundler = joined(inputs->checkout, "dist/bundle/bin.js");
    cJSON *arguments = cJSON_CreateArray();
    int built = bundler != NULL && arguments != NULL &&
                cJSON_AddItemToArray(arguments, cJSON_CreateString(bundler)) &&
                cJSON_AddItemToArray(arguments, cJSON_CreateString("program")) &&
                cJSON_AddItemToArray(arguments, cJSON_CreateString(inputs->directory));
    int exit_code = -1;
    char *output = NULL;
    char *problem = NULL;
    int ran = found == RUNTIME_ON_PATH_FOUND && built &&
              runtime_run_to_exit(node, arguments, BUNDLER_MILLISECONDS, &exit_code, &output, &problem);
    if (found != RUNTIME_ON_PATH_FOUND) {
        fputs("stage-bundle: no node on the PATH runs the bundler\n", stderr);
    } else if (!ran || exit_code != 0) {
        fprintf(stderr, "stage-bundle: %s program %s failed: %s\n", bundler == NULL ? "the bundler" : bundler,
                inputs->directory, ran ? "it exited non-zero" : problem == NULL ? "memory ran out" : problem);
    }
    spisor_memory_release(problem);
    spisor_memory_release(output);
    cJSON_Delete(arguments);
    spisor_memory_release(bundler);
    spisor_memory_release(node);
    cJSON_Delete(environment);
    return ran && exit_code == 0;
}

/** Copies the core's and the standard's shared libraries beside the bundle, named as it looks for them. */
static int copy_weivel_libraries(const char *directory, const char *core, const char *standard) {
    return copy_into(core, directory, SHARED_LIBRARY_PREFIX "weivel_core" SHARED_LIBRARY_ENDING) &&
           copy_into(standard, directory, SHARED_LIBRARY_PREFIX "weivel_standard" SHARED_LIBRARY_ENDING);
}

/**
 * Copies koffi's package from the checkout's node_modules: the files its loader reads and this platform's prebuilt.
 *
 * @implNote koffi names a prebuilt folder by os_arch, the platform key with an underscore, and its sources and every
 * other platform's prebuilt are never loaded, so a bundle built for one platform carries one.
 */
static int copy_koffi(const javascript_host_inputs *inputs) {
    char *package = joined(inputs->checkout, KOFFI_FOLDER);
    char *target = joined(inputs->directory, KOFFI_FOLDER);
    int copied = package != NULL && target != NULL;
    for (size_t index = 0; copied && index < sizeof KOFFI_FILES / sizeof KOFFI_FILES[0]; index++) {
        char *source = joined(package, KOFFI_FILES[index]);
        copied = source != NULL && copy_into(source, target, KOFFI_FILES[index]);
        spisor_memory_release(source);
    }
    char prebuilt[128];
    snprintf(prebuilt, sizeof prebuilt, "build/koffi/%s/koffi.node", runtime_platform_key());
    char *triplet = strchr(prebuilt + strlen("build/koffi/"), '-');
    if (triplet != NULL) {
        *triplet = '_';
    }
    char *source = copied ? joined(package, prebuilt) : NULL;
    copied = source != NULL && copy_into(source, target, prebuilt);
    spisor_memory_release(source);
    spisor_memory_release(target);
    spisor_memory_release(package);
    return copied;
}

/** javascript-host's host.json: its entries and node {pinned, minimum, builds}, the pin catalog data carries. */
static int write_javascript_host_manifest(const javascript_host_inputs *inputs) {
    cJSON *manifest = cJSON_CreateObject();
    cJSON *node = cJSON_CreateObject();
    int built = manifest != NULL && node != NULL && cJSON_AddStringToObject(manifest, "id", "javascript-host") &&
                cJSON_AddStringToObject(manifest, "version", inputs->version) &&
                cJSON_AddStringToObject(manifest, "runtime", "javascript") &&
                cJSON_AddStringToObject(manifest, "entry", "weivel-host.mjs") &&
                cJSON_AddStringToObject(manifest, "inProcessEntry", "weivel-host-in-process.mjs") &&
                cJSON_AddItemToObject(manifest, "node", node);
    if (!built) {
        cJSON_Delete(node);
    }
    built = built &&
            cJSON_AddStringToObject(node, "pinned",
                                    cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(inputs->pin, "pinned"))) &&
            cJSON_AddStringToObject(node, "minimum", inputs->minimum) &&
            cJSON_AddItemToObject(node, "builds",
                                  cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(inputs->pin, "builds"), 1));
    char *text = built ? spisor_json_stringify_pretty(manifest) : NULL;
    char *path = joined(inputs->directory, "host.json");
    int written = text != NULL && path != NULL &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK;
    if (!written) {
        fprintf(stderr, "stage-bundle: the host.json of %s could not be written\n", inputs->directory);
    }
    spisor_memory_release(path);
    spisor_memory_release(text);
    cJSON_Delete(manifest);
    return written;
}

/**
 * javascript-host's bundle: weivel/typescript's program bundled by its own weivel-bundle, the core and standard beside
 * it, koffi under node_modules, and host.json carrying the Node pin with the checkout's minimum.
 */
static int stage_javascript_host(int count, char **arguments) {
    if (count != 8) {
        fputs(USAGE, stderr);
        return 2;
    }
    javascript_host_inputs inputs;
    memset(&inputs, 0, sizeof inputs);
    inputs.directory = arguments[2];
    inputs.version = arguments[3];
    inputs.checkout = arguments[4];
    int staged = read_minimum(&inputs) && read_pin(&inputs, arguments[5]) && start_empty(inputs.directory) &&
                 run_bundler(&inputs) && copy_weivel_libraries(inputs.directory, arguments[6], arguments[7]) &&
                 copy_koffi(&inputs) && write_javascript_host_manifest(&inputs);
    cJSON_Delete(inputs.pin);
    return staged ? 0 : 1;
}

static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "host") == 0) {
        return stage_host(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "library") == 0) {
        return stage_library(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "javascript-host") == 0) {
        return stage_javascript_host(count, arguments);
    }
    fputs(USAGE, stderr);
    return 2;
}

#if defined(_WIN32)

int wmain(int count, wchar_t **wide) {
    char **arguments = host_utf8_arguments(count, wide);
    int code = arguments == NULL ? 2 : run(count, arguments);
    host_free_arguments(arguments, count);
    return code;
}

#else

int main(int count, char **arguments) {
    return run(count, arguments);
}

#endif
