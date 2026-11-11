#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "directory_tree.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include "host_arguments.h"
#define EXECUTABLE_ENDING ".exe"
#else
#include <sys/stat.h>
#define EXECUTABLE_ENDING ""
#endif

#define USAGE                                                                                                         \
    "usage: stage-bundle host <directory> <version> <spisor-host> <spisor-boot> [<runtime-launcher>]\n"                \
    "       stage-bundle library <directory> <library.json> <library binary>\n"

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

static int copy_into(const char *source, const char *directory, const char *name) {
    char *destination = joined(directory, name);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(source, &length);
    int copied = destination != NULL && bytes != NULL && spisor_replace_file(destination, bytes, length) == 1;
#if !defined(_WIN32)
    copied = copied && chmod(destination, 0755) == 0;
#endif
    if (!copied) {
        fprintf(stderr, "stage-bundle: %s could not be copied to %s\n", source, destination == NULL ? name : destination);
    }
    free(bytes);
    free(destination);
    return copied;
}

static int start_empty(const char *directory) {
    if (runtime_remove_tree(directory) != 1 || spisor_make_directories(directory) != 1) {
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
    int written = text != NULL && path != NULL && spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == 1;
    free(path);
    free(text);
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
    int staged = platform != NULL && start_empty(directory) && spisor_make_directories(platform) == 1 &&
                 write_host_manifest(directory, arguments[3]) &&
                 copy_into(arguments[4], platform, "spisor-host" EXECUTABLE_ENDING) &&
                 copy_into(arguments[5], platform, "spisor-boot" EXECUTABLE_ENDING) &&
                 (count == 6 || copy_into(arguments[6], platform, "runtime-launcher.exe"));
    free(platform);
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

static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "host") == 0) {
        return stage_host(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "library") == 0) {
        return stage_library(count, arguments);
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
