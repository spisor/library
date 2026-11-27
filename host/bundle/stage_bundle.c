#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "allocation.h"
#include "cache_refusal.h"
#include "catalog_patterns.h"
#include "catalog_text.h"
#include "directory_tree.h"
#include "hosts_layers.h"
#include "node_json_file.h"
#include "paths.h"
#include "platform_key.h"
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
    "       stage-bundle language-host <directory> <version> <language> <runtime> <entry> <host-data json> "           \
    "<weivel_core> <weivel_standard> [<file or directory>...]\n"                                                      \
    "       stage-bundle interface <directory> <version> <interface id> <binary> <weivel_core> <weivel_standard>\n"   \
    "       stage-bundle platform\n"
#define LANGUAGE_HOST_ARGUMENTS 10
#define INTERFACE_ARGUMENTS 8

/** The members stage-bundle writes into a language host's host.json itself, which its host-data may not replace. */
static const char *const OWN_MEMBERS[] = {"kind", "id", "version", "language", "runtime", "entry"};

static const char *const RUNTIMES[] = {RUNTIME_LAYER_JAVASCRIPT, RUNTIME_LAYER_JVM, RUNTIME_LAYER_NATIVE};

/** A language host or interface bundle being staged: where its files go, `<platform>/` for a native one. */
typedef struct staged_item {
    const char *directory;
    char *files;
    cJSON *manifest;
} staged_item;

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

static int is_one_of(const char *text, const char *const *names, size_t count) {
    for (size_t index = 0; text != NULL && index < count; index++) {
        if (strcmp(text, names[index]) == 0) {
            return 1;
        }
    }
    return 0;
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

/** Copies the file or directory source to name inside directory, a directory with everything under it. */
static int copy_tree_into(const char *source, const char *directory, const char *name) {
    runtime_entry_kind kind = runtime_probe_entry(source);
    if (kind != RUNTIME_ENTRY_DIRECTORY) {
        return copy_into(source, directory, name);
    }
    runtime_directory_listing listing = {NULL, 0};
    int copied = runtime_list_directory(source, &listing) == SPISOR_OK;
    if (!copied) {
        fprintf(stderr, "stage-bundle: %s could not be listed\n", source);
    }
    for (size_t index = 0; copied && index < listing.count; index++) {
        const runtime_directory_entry *entry = &listing.entries[index];
        char *from = joined(source, entry->name);
        char *to = joined(name, entry->name);
        if (!entry->is_directory && !entry->is_file) {
            fprintf(stderr, "stage-bundle: %s is a link, which a bundle cannot carry\n", from == NULL ? source : from);
            copied = 0;
        } else {
            copied = from != NULL && to != NULL && copy_tree_into(from, directory, to);
        }
        spisor_memory_release(to);
        spisor_memory_release(from);
    }
    runtime_free_directory_listing(&listing);
    return copied;
}

static int start_empty(const char *directory) {
    if (runtime_remove_tree(directory) != SPISOR_OK || spisor_make_directories(directory) != SPISOR_OK) {
        fprintf(stderr, "stage-bundle: %s could not be emptied\n", directory);
        return 0;
    }
    return 1;
}

static int write_manifest(const char *directory, const cJSON *manifest) {
    char *text = manifest == NULL ? NULL : spisor_json_stringify_pretty(manifest);
    char *path = joined(directory, "host.json");
    int written = text != NULL && path != NULL &&
                  spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK;
    if (!written) {
        fprintf(stderr, "stage-bundle: the host.json of %s could not be written\n", directory);
    }
    spisor_memory_release(path);
    spisor_memory_release(text);
    return written;
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
    int written = write_manifest(directory, manifest);
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

/** The item's entry as host.json names it, with / separators: name itself, or `<platform>/<name>` per platform. */
static char *entry_named(const char *name, int per_platform) {
    const char *parts[3] = {runtime_platform_key(), "/", name};
    return per_platform ? runtime_cache_text(parts, 3) : runtime_catalog_copy(name);
}

/** Opens item: its manifest begun as {kind, id, version}, its files folder made, the directory emptied first. */
static int begin_item(staged_item *item, const char *directory, const char *kind, const char *id, const char *version,
                      int per_platform) {
    item->directory = directory;
    item->files = per_platform ? joined(directory, runtime_platform_key()) : runtime_catalog_copy(directory);
    item->manifest = cJSON_CreateObject();
    int built = item->files != NULL && item->manifest != NULL &&
                cJSON_AddStringToObject(item->manifest, "kind", kind) != NULL &&
                cJSON_AddStringToObject(item->manifest, "id", id) != NULL &&
                cJSON_AddStringToObject(item->manifest, "version", version) != NULL;
    if (!built) {
        fputs("stage-bundle: memory ran out\n", stderr);
    }
    return built && start_empty(directory) && spisor_make_directories(item->files) == SPISOR_OK;
}

static void end_item(staged_item *item) {
    spisor_memory_release(item->files);
    cJSON_Delete(item->manifest);
}

/** Adds the runtime and the entry to item's manifest, the entry inside its files folder; 0 when memory runs out. */
static int add_start(staged_item *item, const char *runtime, const char *entry, int per_platform) {
    char *named = entry_named(entry, per_platform);
    int added = named != NULL && cJSON_AddStringToObject(item->manifest, "runtime", runtime) != NULL &&
                cJSON_AddStringToObject(item->manifest, "entry", named) != NULL;
    spisor_memory_release(named);
    return added;
}

/** Copies the core's and the standard's shared libraries into the item's files, named as its entry looks for them. */
static int copy_weivel_libraries(const staged_item *item, const char *core, const char *standard) {
    return copy_into(core, item->files, SHARED_LIBRARY_PREFIX "weivel_core" SHARED_LIBRARY_ENDING) &&
           copy_into(standard, item->files, SHARED_LIBRARY_PREFIX "weivel_standard" SHARED_LIBRARY_ENDING);
}

/** Whether path is a relative path inside the working directory, printing what is wrong when it is not. */
static int is_relative_inside(const char *path, const char *what) {
    if (runtime_catalog_is_file_key(path)) {
        return 1;
    }
    fprintf(stderr, "stage-bundle: the %s %s is not a relative path with / separators inside the working directory\n",
            what, path);
    return 0;
}

/** Whether the words name a language, one of the three runtimes and a relative entry, printing why not. */
static int are_language_host_words(char **arguments) {
    const char *language = arguments[4];
    const char *runtime = arguments[5];
    if (!runtime_catalog_is_language(language)) {
        fprintf(stderr, "stage-bundle: %s is no language name: lowercase words of letters and digits joined by -, "
                        "and not spisor\n", language);
        return 0;
    }
    if (!is_one_of(runtime, RUNTIMES, sizeof RUNTIMES / sizeof RUNTIMES[0])) {
        fprintf(stderr, "stage-bundle: %s is no runtime: name javascript, jvm or native\n", runtime);
        return 0;
    }
    return is_relative_inside(arguments[6], "entry");
}

/**
 * The host-data file read: a JSON object naming none of the members stage-bundle writes itself; NULL, printing why,
 * otherwise.
 */
static cJSON *read_host_data(const char *path) {
    int out_of_memory = 0;
    cJSON *data = runtime_read_node_json(path, &out_of_memory);
    if (!cJSON_IsObject(data)) {
        fprintf(stderr, "stage-bundle: the host-data file %s %s\n", path,
                out_of_memory ? "ran out of memory being read" : "is no JSON object");
        cJSON_Delete(data);
        return NULL;
    }
    for (const cJSON *member = data->child; member != NULL; member = member->next) {
        if (is_one_of(member->string, OWN_MEMBERS, sizeof OWN_MEMBERS / sizeof OWN_MEMBERS[0])) {
            fprintf(stderr, "stage-bundle: the host-data file %s names %s, which stage-bundle writes itself\n", path,
                    member->string);
            cJSON_Delete(data);
            return NULL;
        }
    }
    return data;
}

/** Moves every member of data after item's own, in data's order; data is emptied. */
static void merge_host_data(staged_item *item, cJSON *data) {
    while (data->child != NULL) {
        cJSON *member = cJSON_DetachItemViaPointer(data, data->child);
        cJSON_AddItemToObject(item->manifest, member->string, member);
    }
}

/** Copies each file or directory argument from index on into item's files at the same relative path. */
static int copy_files(const staged_item *item, int count, char **arguments, int index) {
    int copied = 1;
    for (; copied && index < count; index++) {
        copied = is_relative_inside(arguments[index], "file") &&
                 copy_tree_into(arguments[index], item->files, arguments[index]);
    }
    return copied;
}

/**
 * A language's host item: host.json {kind: language-host, id: <language>-host, version, language, runtime, entry}
 * followed by the host-data file's members, and the entry, the files and the core and standard in its files folder,
 * which is `<platform>/` for a native one. The entry and the files are relative paths read from the working directory.
 */
static int stage_language_host(int count, char **arguments) {
    if (count < LANGUAGE_HOST_ARGUMENTS) {
        fputs(USAGE, stderr);
        return 2;
    }
    if (!are_language_host_words(arguments)) {
        return 1;
    }
    cJSON *data = read_host_data(arguments[7]);
    if (data == NULL) {
        return 1;
    }
    int per_platform = strcmp(arguments[5], RUNTIME_LAYER_NATIVE) == 0;
    char *id = runtime_language_host_id(arguments[4]);
    staged_item item = {NULL, NULL, NULL};
    int staged = id != NULL && begin_item(&item, arguments[2], "language-host", id, arguments[3], per_platform) &&
                 cJSON_AddStringToObject(item.manifest, "language", arguments[4]) != NULL &&
                 add_start(&item, arguments[5], arguments[6], per_platform);
    if (staged) {
        merge_host_data(&item, data);
    }
    staged = staged && copy_tree_into(arguments[6], item.files, arguments[6]) &&
             copy_weivel_libraries(&item, arguments[8], arguments[9]) &&
             copy_files(&item, count, arguments, LANGUAGE_HOST_ARGUMENTS) &&
             write_manifest(item.directory, item.manifest);
    end_item(&item);
    spisor_memory_release(id);
    cJSON_Delete(data);
    return staged ? 0 : 1;
}

/**
 * An interface item: host.json {kind: interface, id, version, runtime: native, entry} and `<platform>/` holding the
 * binary, under its own name, beside the core and standard.
 */
static int stage_interface(int count, char **arguments) {
    if (count != INTERFACE_ARGUMENTS) {
        fputs(USAGE, stderr);
        return 2;
    }
    if (!runtime_catalog_is_item_id(arguments[4])) {
        fprintf(stderr, "stage-bundle: %s is no item id a catalog can list\n", arguments[4]);
        return 1;
    }
    const char *binary = base_name_of(arguments[5]);
    staged_item item = {NULL, NULL, NULL};
    int staged = begin_item(&item, arguments[2], "interface", arguments[4], arguments[3], 1) &&
                 add_start(&item, RUNTIME_LAYER_NATIVE, binary, 1) && copy_into(arguments[5], item.files, binary) &&
                 copy_weivel_libraries(&item, arguments[6], arguments[7]) &&
                 write_manifest(item.directory, item.manifest);
    end_item(&item);
    return staged ? 0 : 1;
}

/** Prints the platform key a bundle staged here is for, so a build script never computes a second one. */
static int print_platform(int count) {
    if (count != 2) {
        fputs(USAGE, stderr);
        return 2;
    }
    puts(runtime_platform_key());
    return 0;
}

static int run(int count, char **arguments) {
    if (count >= 2 && strcmp(arguments[1], "host") == 0) {
        return stage_host(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "library") == 0) {
        return stage_library(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "language-host") == 0) {
        return stage_language_host(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "interface") == 0) {
        return stage_interface(count, arguments);
    }
    if (count >= 2 && strcmp(arguments[1], "platform") == 0) {
        return print_platform(count);
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
