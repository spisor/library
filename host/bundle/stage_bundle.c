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
#include "loader.h"
#include "node_json_file.h"
#include "paths.h"
#include "platform_key.h"
#include "spisor/file.h"
#include "spisor/json_write.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include "host_arguments.h"
#include "windows_file.h"
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
    "       stage-bundle loader <directory> <standard.json>\n"                                                       \
    "       stage-bundle platform\n"
#define LANGUAGE_HOST_ARGUMENTS 10
#define INTERFACE_ARGUMENTS 8
#define STAGING_ENDING ".staging"
#define PREVIOUS_ENDING ".previous"
#define CORE_LIBRARY SHARED_LIBRARY_PREFIX "weivel_core" SHARED_LIBRARY_ENDING
#define STANDARD_LIBRARY SHARED_LIBRARY_PREFIX "weivel_standard" SHARED_LIBRARY_ENDING

/** The members stage-bundle writes into a language host's host.json itself, which its host-data may not replace. */
static const char *const OWN_MEMBERS[] = {"kind", "id", "version", "language", "runtime", "entry"};

/** The members stage-bundle writes into a loader's host.json itself, which its standard may not name. */
static const char *const LOADER_MEMBERS[] = {"kind", "id", "version", "runtime"};

static const char *const RUNTIMES[] = {RUNTIME_LAYER_JAVASCRIPT, RUNTIME_LAYER_JVM, RUNTIME_LAYER_NATIVE};

/**
 * A language host or interface bundle being staged: built in `staging`, the sibling `<directory>.staging`, whose
 * `files` folder is `<platform>/` for a native one, and swapped into `directory` once every file is in it, the bundle
 * before it moved aside to the sibling `previous`, `<directory>.previous`, until the swap is done.
 */
typedef struct staged_item {
    char *directory;
    char *staging;
    char *previous;
    char *files;
    cJSON *manifest;
    int owns_staging;
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

static int lowered(int character) {
    return character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character;
}

static int equals_ignoring_case(const char *left, const char *right) {
    while (*left != 0 && lowered(*left) == lowered(*right)) {
        left++;
        right++;
    }
    return *left == 0 && *right == 0;
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

/** Whether a real file, or with `folders` a real folder, stands at path itself, printing what stands there when not. */
static int is_real_source(const char *path, const char *what, int folders) {
    runtime_entry_kind kind = runtime_probe_entry(path);
    if (kind == RUNTIME_ENTRY_MISSING) {
        fprintf(stderr, "stage-bundle: the %s %s does not exist\n", what, path);
        return 0;
    }
    if (kind == RUNTIME_ENTRY_DIRECTORY && !folders) {
        fprintf(stderr, "stage-bundle: the %s %s is a folder, not a file\n", what, path);
        return 0;
    }
    if (kind != RUNTIME_ENTRY_DIRECTORY && !runtime_is_real_file(path)) {
        fprintf(stderr, "stage-bundle: the %s %s is a link, which a bundle cannot carry\n", what, path);
        return 0;
    }
    return 1;
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

static int is_version(const char *version) {
    if (runtime_catalog_is_version(version)) {
        return 1;
    }
    fprintf(stderr, "stage-bundle: %s is no version: name <major>.<minor>.<patch>\n", version);
    return 0;
}

/** Whether the core and the standard are real files, as every bundle carries both. */
static int are_libraries(const char *core, const char *standard) {
    return is_real_source(core, "weivel_core", 0) && is_real_source(standard, "weivel_standard", 0);
}

/** Whether the words name a version, a language, a runtime of the three, and an entry and files that can be copied. */
static int are_language_host_words(int count, char **arguments) {
    const char *language = arguments[4];
    const char *runtime = arguments[5];
    if (!is_version(arguments[3])) {
        return 0;
    }
    if (!runtime_catalog_is_language(language)) {
        fprintf(stderr, "stage-bundle: %s is no language name: lowercase words of letters and digits joined by -, "
                        "and not spisor\n", language);
        return 0;
    }
    if (!is_one_of(runtime, RUNTIMES, sizeof RUNTIMES / sizeof RUNTIMES[0])) {
        fprintf(stderr, "stage-bundle: %s is no runtime: name javascript, jvm or native\n", runtime);
        return 0;
    }
    int usable = is_relative_inside(arguments[6], "entry") && is_real_source(arguments[6], "entry", 0) &&
                 are_libraries(arguments[8], arguments[9]);
    for (int index = LANGUAGE_HOST_ARGUMENTS; usable && index < count; index++) {
        usable = is_relative_inside(arguments[index], "file") && is_real_source(arguments[index], "file", 1);
    }
    return usable;
}

/** Whether the binary's own name can be a file a catalog lists and is neither library it is staged beside. */
static int is_binary_name(const char *name) {
    if (!runtime_catalog_is_name(name) || !runtime_catalog_is_file_key(name)) {
        fprintf(stderr, "stage-bundle: the binary's name %s is no file name a catalog can list\n", name);
        return 0;
    }
    if (equals_ignoring_case(name, CORE_LIBRARY) || equals_ignoring_case(name, STANDARD_LIBRARY)) {
        fprintf(stderr, "stage-bundle: the binary's name %s is the name of a library the bundle carries beside it\n",
                name);
        return 0;
    }
    return 1;
}

/** Whether the words name an item id, a version, and a binary and libraries that can be copied. */
static int are_interface_words(char **arguments) {
    if (!is_version(arguments[3])) {
        return 0;
    }
    if (!runtime_catalog_is_item_id(arguments[4])) {
        fprintf(stderr, "stage-bundle: %s is no item id a catalog can list\n", arguments[4]);
        return 0;
    }
    return is_real_source(arguments[5], "binary", 0) && is_binary_name(base_name_of(arguments[5])) &&
           are_libraries(arguments[6], arguments[7]);
}

/** Whether the host-data member is one stage-bundle refuses, printing why. */
static int is_refused_member(const cJSON *member, const char *path) {
    if (is_one_of(member->string, OWN_MEMBERS, sizeof OWN_MEMBERS / sizeof OWN_MEMBERS[0])) {
        fprintf(stderr, "stage-bundle: the host-data file %s names %s, which stage-bundle writes itself\n", path,
                member->string);
        return 1;
    }
    if (strcmp(member->string, "application") == 0) {
        fprintf(stderr, "stage-bundle: the host-data file %s names application, which only a loader's host.json "
                        "carries\n", path);
        return 1;
    }
    return 0;
}

/**
 * Whether the host-data text names one member twice, printing which.
 *
 * @implNote Read with cJSON's own parser, which keeps every member as written, since the JSON.parse reading of the file
 * keeps only the last of two and could not tell.
 */
static int names_a_member_twice(const uint8_t *bytes, size_t length, const char *path, const char *what) {
    cJSON *written = cJSON_ParseWithLength((const char *)bytes, length);
    int twice = 0;
    for (const cJSON *member = written == NULL ? NULL : written->child; !twice && member != NULL;
         member = member->next) {
        for (const cJSON *earlier = written->child; !twice && earlier != member; earlier = earlier->next) {
            twice = member->string != NULL && earlier->string != NULL && strcmp(earlier->string, member->string) == 0;
        }
        if (twice) {
            fprintf(stderr, "stage-bundle: the %s %s names %s twice\n", what, path, member->string);
        }
    }
    cJSON_Delete(written);
    return twice;
}

/** The JSON object the file `what` at path holds, naming each member once; NULL, printing why, otherwise. */
static cJSON *read_object(const char *path, const char *what) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    int out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    cJSON *data = bytes == NULL ? NULL : runtime_parse_node_json(bytes, length, &out_of_memory);
    int twice = cJSON_IsObject(data) && names_a_member_twice(bytes, length, path, what);
    spisor_memory_release(bytes);
    if (twice) {
        cJSON_Delete(data);
        return NULL;
    }
    if (bytes == NULL || !cJSON_IsObject(data)) {
        fprintf(stderr, "stage-bundle: the %s %s %s\n", what, path,
                out_of_memory ? "ran out of memory being read"
                : bytes == NULL ? "could not be read"
                                : "is no JSON object");
        cJSON_Delete(data);
        return NULL;
    }
    return data;
}

/**
 * The host-data file read: a JSON object naming each member once, and neither application nor any member stage-bundle
 * writes itself; NULL, printing why, otherwise.
 */
static cJSON *read_host_data(const char *path) {
    cJSON *data = read_object(path, "host-data file");
    for (const cJSON *member = data == NULL ? NULL : data->child; member != NULL; member = member->next) {
        if (is_refused_member(member, path)) {
            cJSON_Delete(data);
            return NULL;
        }
    }
    return data;
}

/** The item's entry as host.json names it, with / separators: name itself, or `<platform>/<name>` per platform. */
static char *entry_named(const char *name, int per_platform) {
    const char *parts[3] = {runtime_platform_key(), "/", name};
    return per_platform ? runtime_cache_text(parts, 3) : runtime_catalog_copy(name);
}

static int is_separator(char character) {
    return character == '/' || (RUNTIME_HOST_IS_WINDOWS && character == '\\');
}

/**
 * The bundle's folder with its trailing separators taken off, so its staging and previous folders are siblings of it;
 * NULL, printing why, when what is left names no folder of its own: a root, a drive, `.` or `..`.
 */
static char *bundle_folder(const char *directory) {
    char *folder = runtime_catalog_copy(directory);
    if (folder == NULL) {
        fputs("stage-bundle: memory ran out\n", stderr);
        return NULL;
    }
    size_t length = strlen(folder);
    while (length > 0 && is_separator(folder[length - 1])) {
        folder[--length] = 0;
    }
    size_t start = length;
    while (start > 0 && !is_separator(folder[start - 1])) {
        start--;
    }
    const char *segment = folder + start;
    int share_root = RUNTIME_HOST_IS_WINDOWS && start > 2 && is_separator(folder[0]) && is_separator(folder[1]) &&
                     strpbrk(folder + 2, "/\\") == folder + start - 1;
    int own = *segment != 0 && strcmp(segment, ".") != 0 && strcmp(segment, "..") != 0 && !share_root &&
              !(RUNTIME_HOST_IS_WINDOWS && strchr(segment, ':') != NULL);
    if (!own) {
        fprintf(stderr, "stage-bundle: %s names no folder of its own: name the bundle's folder, not a root, . or ..\n",
                directory);
        spisor_memory_release(folder);
        return NULL;
    }
    return folder;
}

/** Whether the real folder path is empty or holds host.json as a real file, as every bundle stage-bundle writes. */
static int holds_a_manifest_or_nothing(const char *path) {
    runtime_directory_listing listing = {NULL, 0};
    int empty = runtime_list_directory(path, &listing) == SPISOR_OK && listing.count == 0;
    runtime_free_directory_listing(&listing);
    char *manifest = empty ? NULL : joined(path, "host.json");
    int manifested = manifest != NULL && runtime_is_real_file(manifest);
    spisor_memory_release(manifest);
    return empty || manifested;
}

/**
 * Whether stage-bundle may replace what stands at path: nothing, or a real folder that is empty or holds a top-level
 * host.json; printing why not.
 *
 * @implNote A folder that is no bundle is never moved or removed, whatever it was named: a shell that rewrites `/`
 * into an install folder once led stage-bundle to swap that install aside.
 */
static int is_replaceable(const char *path, const char *what) {
    runtime_entry_kind kind = runtime_probe_entry(path);
    if (kind == RUNTIME_ENTRY_MISSING) {
        return 1;
    }
    if (kind != RUNTIME_ENTRY_DIRECTORY) {
        fprintf(stderr, "stage-bundle: the %s %s is no folder: it is a link or a file, which stage-bundle never "
                        "replaces\n", what, path);
        return 0;
    }
    if (!holds_a_manifest_or_nothing(path)) {
        fprintf(stderr, "stage-bundle: the %s folder %s holds no host.json, so it is no bundle stage-bundle replaces\n",
                what, path);
        return 0;
    }
    return 1;
}

/**
 * Empties item's staging folder, from then on its own, and removes a previous folder a crash left, after checking
 * that each, and the bundle's own folder, is one stage-bundle may replace; the staging folder gets host.json at once,
 * so a run that dies leaves one the next run recognises.
 */
static int clear_siblings(staged_item *item) {
    if (!is_replaceable(item->directory, "bundle") || !is_replaceable(item->staging, "staging") ||
        !is_replaceable(item->previous, "previous")) {
        return 0;
    }
    if (runtime_remove_tree(item->previous) != SPISOR_OK) {
        fprintf(stderr, "stage-bundle: the previous folder %s could not be removed\n", item->previous);
        return 0;
    }
    item->owns_staging = start_empty(item->staging);
    return item->owns_staging && write_manifest(item->staging, item->manifest);
}

/** Opens item: its manifest begun as {kind, id, version} and its files folder made inside an empty staging folder. */
static int begin_item(staged_item *item, const char *directory, const char *kind, const char *id, const char *version,
                      int per_platform) {
    item->directory = bundle_folder(directory);
    if (item->directory == NULL) {
        return 0;
    }
    const char *parts[2] = {item->directory, STAGING_ENDING};
    const char *previous_parts[2] = {item->directory, PREVIOUS_ENDING};
    item->staging = runtime_cache_text(parts, 2);
    item->previous = runtime_cache_text(previous_parts, 2);
    item->files = item->staging == NULL ? NULL
                  : per_platform        ? joined(item->staging, runtime_platform_key())
                                        : runtime_catalog_copy(item->staging);
    item->manifest = cJSON_CreateObject();
    int built = item->files != NULL && item->previous != NULL && item->manifest != NULL &&
                cJSON_AddStringToObject(item->manifest, "kind", kind) != NULL &&
                cJSON_AddStringToObject(item->manifest, "id", id) != NULL &&
                cJSON_AddStringToObject(item->manifest, "version", version) != NULL;
    if (!built) {
        fputs("stage-bundle: memory ran out\n", stderr);
    }
    return built && clear_siblings(item) && spisor_make_directories(item->files) == SPISOR_OK;
}

static int rename_directory(const char *from, const char *to) {
#if defined(_WIN32)
    return spisor_windows_move_directory(from, to);
#else
    return rename(from, to) == 0;
#endif
}

/**
 * Renames the staging folder to the bundle's folder.
 *
 * @implNote The failing build of stage-bundle, which only the suite builds, fails this one rename, the step after the
 * bundle before has moved aside, so a test can watch that bundle put back.
 */
static int rename_into_place(const char *staging, const char *directory) {
#if defined(STAGE_BUNDLE_FAILING_RENAME_INTO_PLACE)
    (void)staging;
    (void)directory;
    return 0;
#else
    return rename_directory(staging, directory);
#endif
}

/**
 * Writes item's host.json into its staging folder and swaps it into the bundle's folder: the bundle before moved aside
 * to the previous folder, the staging folder renamed into place, and the previous folder removed, or renamed back when
 * the staging folder could not be. A previous folder that cannot be removed after the swap is only warned about, since
 * the next staging removes it.
 */
static int finish_item(const staged_item *item) {
    if (!write_manifest(item->staging, item->manifest)) {
        return 0;
    }
    int replacing = runtime_probe_entry(item->directory) != RUNTIME_ENTRY_MISSING;
    if (replacing && !rename_directory(item->directory, item->previous)) {
        fprintf(stderr, "stage-bundle: %s could not be renamed aside to %s\n", item->directory, item->previous);
        return 0;
    }
    if (!rename_into_place(item->staging, item->directory)) {
        int restored = !replacing || rename_directory(item->previous, item->directory);
        fprintf(stderr, "stage-bundle: %s could not be renamed over %s%s\n", item->staging, item->directory,
                restored ? "" : ", and the bundle before it stays in the previous folder");
        return 0;
    }
    if (replacing && runtime_remove_tree(item->previous) != SPISOR_OK) {
        fprintf(stderr, "stage-bundle: warning: %s could not be removed; the next staging removes it\n",
                item->previous);
    }
    return 1;
}

/** Frees item, removing the staging folder it made when it was not renamed, so a refusal leaves what stood before. */
static void end_item(staged_item *item) {
    if (item->owns_staging && runtime_probe_entry(item->staging) == RUNTIME_ENTRY_DIRECTORY) {
        runtime_remove_tree(item->staging);
    }
    spisor_memory_release(item->staging);
    spisor_memory_release(item->previous);
    spisor_memory_release(item->directory);
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
    return copy_into(core, item->files, CORE_LIBRARY) && copy_into(standard, item->files, STANDARD_LIBRARY);
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
        copied = copy_tree_into(arguments[index], item->files, arguments[index]);
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
    if (!are_language_host_words(count, arguments)) {
        return 1;
    }
    cJSON *data = read_host_data(arguments[7]);
    if (data == NULL) {
        return 1;
    }
    int per_platform = strcmp(arguments[5], RUNTIME_LAYER_NATIVE) == 0;
    char *id = runtime_language_host_id(arguments[4]);
    staged_item item = {NULL, NULL, NULL, NULL, NULL, 0};
    int staged = id != NULL && begin_item(&item, arguments[2], "language-host", id, arguments[3], per_platform) &&
                 cJSON_AddStringToObject(item.manifest, "language", arguments[4]) != NULL &&
                 add_start(&item, arguments[5], arguments[6], per_platform);
    if (staged) {
        merge_host_data(&item, data);
    }
    staged = staged && copy_tree_into(arguments[6], item.files, arguments[6]) &&
             copy_weivel_libraries(&item, arguments[8], arguments[9]) &&
             copy_files(&item, count, arguments, LANGUAGE_HOST_ARGUMENTS) && finish_item(&item);
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
    if (!are_interface_words(arguments)) {
        return 1;
    }
    const char *binary = base_name_of(arguments[5]);
    staged_item item = {NULL, NULL, NULL, NULL, NULL, 0};
    int staged = begin_item(&item, arguments[2], "interface", arguments[4], arguments[3], 1) &&
                 add_start(&item, RUNTIME_LAYER_NATIVE, binary, 1) && copy_into(arguments[5], item.files, binary) &&
                 copy_weivel_libraries(&item, arguments[6], arguments[7]) && finish_item(&item);
    end_item(&item);
    return staged ? 0 : 1;
}

/** The file name in the folder holding path, which is the working directory when path names no folder. */
static char *beside(const char *path, const char *name) {
    size_t folder = (size_t)(base_name_of(path) - path);
    const char *parts[2] = {path, name};
    char *text = runtime_cache_text(parts, 2);
    if (text != NULL) {
        memmove(text + folder, text + strlen(path), strlen(name) + 1);
    }
    return text;
}

/** The package.json beside the standard, its name an item id and its version one a catalog lists; NULL, printing why. */
static cJSON *read_loader_package(const char *standard_path) {
    char *path = beside(standard_path, "package.json");
    cJSON *package = path == NULL ? NULL : read_object(path, "package file");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(package, "name");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(package, "version");
    int usable = package != NULL;
    if (usable && !(cJSON_IsString(name) && runtime_catalog_is_item_id(name->valuestring))) {
        fprintf(stderr, "stage-bundle: the package file %s names %s, which is no item id a catalog can list: name the "
                        "loader without a scope\n", path, cJSON_IsString(name) ? name->valuestring : "no name");
        usable = 0;
    }
    if (usable && !cJSON_IsString(version)) {
        fprintf(stderr, "stage-bundle: the package file %s names no version\n", path);
        usable = 0;
    }
    usable = usable && is_version(version->valuestring);
    spisor_memory_release(path);
    if (!usable) {
        cJSON_Delete(package);
        return NULL;
    }
    return package;
}

/** Whether standard is a loader standard naming none of the members stage-bundle writes, printing the first issue. */
static int is_loader_standard(const cJSON *standard, const char *path) {
    for (const cJSON *member = standard->child; member != NULL; member = member->next) {
        if (is_one_of(member->string, LOADER_MEMBERS, sizeof LOADER_MEMBERS / sizeof LOADER_MEMBERS[0])) {
            fprintf(stderr, "stage-bundle: the standard %s names %s, which stage-bundle writes itself\n", path,
                    member->string);
            return 0;
        }
    }
    cJSON *issues = runtime_loader_standard_issues(standard);
    const cJSON *first = cJSON_GetArrayItem(issues, 0);
    if (issues == NULL) {
        fputs("stage-bundle: memory ran out\n", stderr);
    } else if (first != NULL) {
        fprintf(stderr, "stage-bundle: the standard %s is no loader standard at %s: %s; %s\n", path,
                cJSON_GetObjectItemCaseSensitive(first, "path")->valuestring,
                cJSON_GetObjectItemCaseSensitive(first, "detail")->valuestring,
                cJSON_GetObjectItemCaseSensitive(first, "fix")->valuestring);
    }
    int valid = issues != NULL && first == NULL;
    cJSON_Delete(issues);
    return valid;
}

/**
 * A loader's bundle, its host.json alone: {kind: host, id, version, runtime: native} followed by the members of the
 * standard.json it is staged from, in their order, the id and version read from the package.json beside that file.
 */
static int stage_loader(int count, char **arguments) {
    if (count != 4) {
        fputs(USAGE, stderr);
        return 2;
    }
    cJSON *standard = read_object(arguments[3], "standard");
    cJSON *package = standard != NULL && is_loader_standard(standard, arguments[3]) ? read_loader_package(arguments[3])
                                                                                     : NULL;
    const char *id = package == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(package, "name")->valuestring;
    const char *version = package == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(package, "version")->valuestring;
    staged_item item = {NULL, NULL, NULL, NULL, NULL, 0};
    int staged = package != NULL && begin_item(&item, arguments[2], "host", id, version, 0) &&
                 cJSON_AddStringToObject(item.manifest, "runtime", RUNTIME_LAYER_NATIVE) != NULL;
    if (staged) {
        merge_host_data(&item, standard);
    }
    staged = staged && finish_item(&item);
    end_item(&item);
    cJSON_Delete(package);
    cJSON_Delete(standard);
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
    if (count >= 2 && strcmp(arguments[1], "loader") == 0) {
        return stage_loader(count, arguments);
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
