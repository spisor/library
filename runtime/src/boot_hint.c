#include "boot_hint.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "cache_folders.h"
#include "cache_place.h"
#include "cache_refusal.h"
#include "directory_tree.h"
#include "json_service.h"
#include "host_references.h"
#include "identity.h"
#include "node_json_file.h"
#include "user_home.h"
#include "spisor/file.h"
#include "spisor/json_write.h"

#define WINDOWS_PLATFORM_PREFIX "win32-"
#define DARWIN_PLATFORM_PREFIX "darwin-"
#define WINDOWS_BINARY_SUFFIX ".dll"
#define DARWIN_BINARY_SUFFIX ".dylib"
#define OTHER_BINARY_SUFFIX ".so"

static char *hosts_folder(const char *cache) {
    const char *segments[2] = {cache, "hosts"};
    return runtime_cache_joined(segments, 2);
}

char *runtime_boot_hint_path(const char *cache, const char *host_id, char **problem) {
    *problem = NULL;
    char *folder = hosts_folder(cache);
    char *path = folder == NULL ? NULL : runtime_host_file_path(folder, host_id, "libraries", problem);
    spisor_memory_release(folder);
    return path;
}

static int is_optional_text(const cJSON *value) {
    return value == NULL || cJSON_IsString(value);
}

static int is_hinted_library(const cJSON *entry) {
    return cJSON_IsObject(entry) && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "version")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(entry, "directory")) &&
           is_optional_text(cJSON_GetObjectItemCaseSensitive(entry, "binary"));
}

static int copy_text_member(cJSON *into, const cJSON *from, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(from, member);
    return value == NULL || cJSON_AddStringToObject(into, member, value->valuestring) != NULL;
}

static cJSON *hinted_library(const cJSON *entry) {
    cJSON *library = is_hinted_library(entry) ? cJSON_CreateObject() : NULL;
    if (library != NULL &&
        !(copy_text_member(library, entry, "version") && copy_text_member(library, entry, "directory") &&
          copy_text_member(library, entry, "binary"))) {
        cJSON_Delete(library);
        library = NULL;
    }
    return library;
}

static cJSON *hinted_libraries(const cJSON *value) {
    cJSON *libraries = cJSON_IsObject(value) ? cJSON_CreateObject() : NULL;
    for (const cJSON *entry = libraries == NULL ? NULL : value->child; entry != NULL; entry = entry->next) {
        cJSON *library = hinted_library(entry);
        if (library == NULL || !cJSON_AddItemToObject(libraries, entry->string, library)) {
            cJSON_Delete(library);
            cJSON_Delete(libraries);
            return NULL;
        }
    }
    return libraries;
}

static int is_hint(const cJSON *recorded) {
    const cJSON *libraries = cJSON_GetObjectItemCaseSensitive(recorded, "libraries");
    if (!cJSON_IsObject(recorded) || !cJSON_IsObject(libraries)) {
        return 0;
    }
    for (const cJSON *entry = libraries->child; entry != NULL; entry = entry->next) {
        if (!is_hinted_library(entry)) {
            return 0;
        }
    }
    return 1;
}

cJSON *runtime_boot_hint_of(const cJSON *recorded, int *out_of_memory) {
    *out_of_memory = 0;
    if (!is_hint(recorded)) {
        return NULL;
    }
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(recorded, "host");
    cJSON *hint = cJSON_IsObject(recorded) ? cJSON_CreateObject() : NULL;
    cJSON *libraries = hint == NULL ? NULL : hinted_libraries(cJSON_GetObjectItemCaseSensitive(recorded, "libraries"));
    int attached = libraries != NULL && cJSON_AddItemToObject(hint, "libraries", libraries);
    if (!attached) {
        cJSON_Delete(libraries);
    }
    int built = attached &&
                (!cJSON_IsString(host) || cJSON_AddStringToObject(hint, "host", host->valuestring) != NULL);
    if (!built) {
        cJSON_Delete(hint);
        *out_of_memory = 1;
        return NULL;
    }
    return hint;
}

static int has_prefix(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

typedef struct binary_suffix {
    const char *text;
    int ignores_case;
} binary_suffix;

static int has_suffix(const char *text, binary_suffix suffix) {
    size_t length = strlen(text);
    size_t suffix_length = strlen(suffix.text);
    if (length <= suffix_length) {
        return 0;
    }
    const char *tail = text + length - suffix_length;
    return suffix.ignores_case ? runtime_equal_ignoring_ascii_case(tail, suffix.text) : strcmp(tail, suffix.text) == 0;
}

/** The suffix a binary for platform carries; a Windows file name is matched in any case, as Windows matches it. */
static binary_suffix binary_suffix_of(const char *platform) {
    if (has_prefix(platform, WINDOWS_PLATFORM_PREFIX)) {
        return (binary_suffix){WINDOWS_BINARY_SUFFIX, 1};
    }
    const char *text = has_prefix(platform, DARWIN_PLATFORM_PREFIX) ? DARWIN_BINARY_SUFFIX : OTHER_BINARY_SUFFIX;
    return (binary_suffix){text, 0};
}

/** cache/libraries/<id folder>/<version>/<platform>, resolved; NULL with *problem set when it would leave the id's folder. */
static char *library_directory(const char *cache, const char *id, const char *version, const char *platform,
                               char **problem) {
    const char *root_segments[2] = {"libraries", NULL};
    char *root = runtime_cache_resolve(cache, root_segments, 1);
    int unfit = 0;
    root_segments[1] = root == NULL ? NULL : runtime_item_folder(id, &unfit);
    char *id_root = root_segments[1] == NULL ? NULL : runtime_cache_resolve(cache, root_segments, 2);
    const char *segments[2] = {version, platform};
    char *directory = id_root == NULL ? NULL : runtime_cache_resolve(id_root, segments, 2);
    int inside = directory != NULL && version[0] != 0 && runtime_cache_is_strictly_inside(id_root, directory);
    if (root != NULL && (unfit || (directory != NULL && !inside))) {
        const char *parts[5] = {id, "@", version, " names no library folder inside ", root};
        *problem = runtime_cache_text(parts, 5);
    }
    if (!inside) {
        spisor_memory_release(directory);
        directory = NULL;
    }
    spisor_memory_release(id_root);
    spisor_memory_release((char *)root_segments[1]);
    spisor_memory_release(root);
    return directory;
}

typedef struct binary_search {
    char *first_name;
    size_t count;
} binary_search;

/**
 * The files directly inside directory that end in suffix, a link never counting as one: how many, and the first one's
 * name. 0 with *problem naming directory when it cannot be listed, and with *problem NULL when memory runs out.
 */
static int search_binaries(const char *directory, binary_suffix suffix, binary_search *search, char **problem) {
    search->first_name = NULL;
    search->count = 0;
    runtime_directory_listing listing;
    spisor_status listed = runtime_list_directory(directory, &listing);
    if (listed != SPISOR_OK) {
        const char *parts[2] = {"could not list ", directory};
        *problem = listed == SPISOR_OUT_OF_MEMORY ? NULL : runtime_cache_text(parts, 2);
        return 0;
    }
    int searched = 1;
    for (size_t index = 0; searched && index < listing.count; index++) {
        const runtime_directory_entry *entry = &listing.entries[index];
        if (entry->is_file && has_suffix(entry->name, suffix) && search->count++ == 0) {
            const char *parts[1] = {entry->name};
            search->first_name = runtime_cache_text(parts, 1);
            searched = search->first_name != NULL;
        }
    }
    runtime_free_directory_listing(&listing);
    return searched;
}

/** 1 when directory holds a library declaration, 0 when it holds none, and -1 when memory runs out. */
static int is_declared_library(const char *directory) {
    const char *segments[2] = {directory, RUNTIME_LIBRARY_DECLARATION};
    char *declaration = runtime_cache_joined(segments, 2);
    spisor_file_presence presence = declaration == NULL ? SPISOR_FILE_OUT_OF_MEMORY : spisor_probe_file(declaration);
    spisor_memory_release(declaration);
    return presence == SPISOR_FILE_OUT_OF_MEMORY ? -1 : presence == SPISOR_FILE_PRESENT;
}

/**
 * Adds the one binary of the declared library in directory to entry; 0 with *problem set when it holds none
 * or several, or cannot be listed.
 */
static int add_binary(cJSON *entry, const char *directory, const char *platform, char **problem) {
    binary_suffix suffix = binary_suffix_of(platform);
    binary_search search;
    int searched = search_binaries(directory, suffix, &search, problem);
    if (searched && search.count != 1) {
        const char *parts[5] = {"the library folder ", directory,
                                search.count == 0 ? " holds no " : " holds more than one ", suffix.text,
                                " file beside its " RUNTIME_LIBRARY_DECLARATION};
        *problem = runtime_cache_text(parts, 5);
    }
    const char *segments[2] = {directory, search.first_name};
    char *binary = searched && search.count == 1 ? runtime_cache_joined(segments, 2) : NULL;
    int added = binary != NULL && cJSON_AddStringToObject(entry, "binary", binary) != NULL;
    spisor_memory_release(binary);
    spisor_memory_release(search.first_name);
    return added;
}

static cJSON *library_entry(const char *cache, const char *id, const char *version, const char *platform,
                            char **problem) {
    char *directory = library_directory(cache, id, version, platform, problem);
    cJSON *entry = directory == NULL ? NULL : cJSON_CreateObject();
    int declared = entry == NULL ? -1 : is_declared_library(directory);
    int built = declared >= 0 && cJSON_AddStringToObject(entry, "version", version) != NULL &&
                cJSON_AddStringToObject(entry, "directory", directory) != NULL &&
                (declared == 0 || add_binary(entry, directory, platform, problem));
    if (!built) {
        cJSON_Delete(entry);
        entry = NULL;
    }
    spisor_memory_release(directory);
    return entry;
}

cJSON *runtime_boot_hint_libraries(const char *cache, const cJSON *versions, const char *platform, char **problem) {
    *problem = NULL;
    cJSON *libraries = cJSON_CreateObject();
    for (const cJSON *version = versions->child; libraries != NULL && version != NULL; version = version->next) {
        cJSON *entry = cJSON_IsString(version)
                           ? library_entry(cache, version->string, version->valuestring, platform, problem)
                           : NULL;
        if (entry == NULL || !cJSON_AddItemToObject(libraries, version->string, entry)) {
            cJSON_Delete(entry);
            cJSON_Delete(libraries);
            libraries = NULL;
        }
    }
    return libraries;
}

cJSON *runtime_read_boot_hint(const char *cache, const char *host_id, int *out_of_memory) {
    char *problem = NULL;
    char *path = runtime_boot_hint_path(cache, host_id, &problem);
    *out_of_memory = path == NULL && problem == NULL;
    spisor_memory_release(problem);
    cJSON *recorded = path == NULL ? NULL : runtime_read_node_json(path, out_of_memory);
    spisor_memory_release(path);
    cJSON *hint = *out_of_memory ? NULL : runtime_boot_hint_of(recorded, out_of_memory);
    cJSON_Delete(recorded);
    return hint;
}

/**
 * The host version a write records: the one given, else the one the hint already names, else none, NULL with
 * *out_of_memory set to 1 when memory runs out.
 */
static char *recorded_host(const char *cache, const char *host_id, const char *host, int *out_of_memory) {
    *out_of_memory = 0;
    cJSON *hint = host == NULL ? runtime_read_boot_hint(cache, host_id, out_of_memory) : NULL;
    const cJSON *kept = cJSON_GetObjectItemCaseSensitive(hint, "host");
    const char *version = host != NULL ? host : cJSON_IsString(kept) ? kept->valuestring : NULL;
    const char *parts[1] = {version};
    char *copy = version == NULL ? NULL : runtime_cache_text(parts, 1);
    *out_of_memory = *out_of_memory || (version != NULL && copy == NULL);
    cJSON_Delete(hint);
    return copy;
}

static char *hint_text(const cJSON *libraries, const char *host) {
    cJSON *hint = cJSON_CreateObject();
    int built = hint != NULL && runtime_add_reference(hint, "libraries", libraries) &&
                (host == NULL || cJSON_AddStringToObject(hint, "host", host) != NULL);
    char *pretty = built ? spisor_json_stringify_pretty(hint) : NULL;
    cJSON_Delete(hint);
    const char *parts[2] = {pretty, "\n"};
    char *text = pretty == NULL ? NULL : runtime_cache_text(parts, 2);
    spisor_memory_release(pretty);
    return text;
}

/** Writes the hint's text; 0 with *out_of_memory set to 1 when memory ran out making its folder or writing it. */
static int write_text(const char *cache, const char *path, const char *text, int *out_of_memory) {
    char *folder = hosts_folder(cache);
    spisor_status status = folder == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(folder);
    if (status == SPISOR_OK) {
        status = spisor_replace_file(path, (const uint8_t *)text, strlen(text));
    }
    *out_of_memory = status == SPISOR_OUT_OF_MEMORY;
    spisor_memory_release(folder);
    return status == SPISOR_OK;
}

int runtime_write_boot_hint(const char *cache, const char *host_id, const cJSON *libraries, const char *host,
                            char **problem) {
    char *path = runtime_boot_hint_path(cache, host_id, problem);
    int out_of_memory = 0;
    char *version = path == NULL ? NULL : recorded_host(cache, host_id, host, &out_of_memory);
    char *text = path == NULL || out_of_memory ? NULL : hint_text(libraries, version);
    int written = text != NULL && write_text(cache, path, text, &out_of_memory);
    if (!written && path != NULL && text != NULL && !out_of_memory) {
        const char *parts[3] = {"the boot hint ", path, " could not be written"};
        *problem = runtime_cache_text(parts, 3);
    }
    spisor_memory_release(text);
    spisor_memory_release(version);
    spisor_memory_release(path);
    return written;
}

spisor_status runtime_hint_host_version(const char *cache, const char *host_id, const char *version) {
    int out_of_memory;
    cJSON *hint = runtime_read_boot_hint(cache, host_id, &out_of_memory);
    if (hint == NULL) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
    }
    char *problem = NULL;
    int written = runtime_write_boot_hint(cache, host_id, cJSON_GetObjectItemCaseSensitive(hint, "libraries"),
                                          version, &problem);
    spisor_status status = written ? SPISOR_OK : problem == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
    spisor_memory_release(problem);
    cJSON_Delete(hint);
    return status;
}
