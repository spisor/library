#include "home_prepare.h"

#include <string.h>

#include "boot_hint.h"
#include "cache_place.h"
#include "config.h"
#include "directory_tree.h"
#include "paths.h"
#include "registry.h"
#include "spisor/allocation.h"
#include "spisor/definition.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"
#include "spisor/json_value.h"
#include "spisor/path.h"

#define KOFFI_ID "koffi"
#define DEFAULT_KOFFI_VERSION "2.0.0"
#define BASE_FIX "pass the absolute path of an empty folder that exists"
#define INSTALL_FIX                                                                                                   \
    "give each install a plugin.json record with an id, a version and an entry below its bundle, an absolute entry " \
    "source that exists, and settings that are a record or NULL"
#define LIBRARY_FIX "give each library an id, an absolute directory, and a version or a library.json declaring one"

static spisor_status refuse(spisor_refusal *refusal, const char *detail, const char *fix) {
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail, fix);
}

static spisor_status check_base(const char *base, spisor_refusal *refusal) {
    runtime_directory_listing listing = {NULL, 0};
    runtime_entry_kind kind;
    spisor_status listed;
    if (base == NULL || !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, base)) {
        return refuse(refusal, "the base is absent or not an absolute path", BASE_FIX);
    }
    kind = runtime_probe_entry(base);
    if (kind == RUNTIME_ENTRY_PROBE_OUT_OF_MEMORY) {
        return SPISOR_OUT_OF_MEMORY;
    }
    if (kind != RUNTIME_ENTRY_DIRECTORY) {
        return refuse(refusal, "the base is no folder that exists", BASE_FIX);
    }
    listed = runtime_list_directory(base, &listing);
    if (listed == SPISOR_OK && listing.count > 0) {
        listed = refuse(refusal, "the base is not empty", BASE_FIX);
    } else if (listed == SPISOR_REFUSED) {
        listed = refuse(refusal, "the base cannot be listed", BASE_FIX);
    }
    runtime_free_directory_listing(&listing);
    return listed;
}

static const char *text_member(const cJSON *object, const char *name) {
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
    return text == NULL || text[0] == '\0' ? NULL : text;
}

/** A RECORD as JSON into *json; MALFORMED for anything else, NULL included unless `optional`. */
static spisor_status record_json(const spisor_value *value, int optional, cJSON **json) {
    *json = NULL;
    if (value == NULL) {
        return optional ? SPISOR_OK : SPISOR_MALFORMED;
    }
    if (value->kind != SPISOR_VALUE_RECORD) {
        return SPISOR_MALFORMED;
    }
    return spisor_json_of_value(value, json);
}

static int is_entry_source(const char *path) {
    return path != NULL && spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, path) &&
           spisor_probe_file(path) == SPISOR_FILE_PRESENT;
}

/** The entry's path inside the bundle into prepared->entry; MALFORMED when it climbs out. */
static spisor_status place_entry(runtime_testing_prepared_install *prepared, const char *entry) {
    char *bundle = runtime_cache_resolve(prepared->bundle, NULL, 0);
    prepared->entry = bundle == NULL ? NULL : runtime_cache_resolve(bundle, &entry, 1);
    spisor_status status = prepared->entry == NULL ? SPISOR_OUT_OF_MEMORY
                           : runtime_cache_is_strictly_inside(bundle, prepared->entry) ? SPISOR_OK
                                                                                     : SPISOR_MALFORMED;
    spisor_memory_release(bundle);
    return status;
}

static spisor_status place_bundle(runtime_testing_prepared_install *prepared, const char *cache, const char *entry) {
    int out_of_memory = 0;
    prepared->bundle = runtime_cache_bundle_directory(cache, prepared->id, prepared->version, &out_of_memory);
    if (prepared->bundle == NULL) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_MALFORMED;
    }
    return place_entry(prepared, entry);
}

static spisor_status place_settings_file(runtime_testing_prepared_install *prepared,
                                         const runtime_testing_layout *layout, spisor_refusal *refusal) {
    runtime_config_request request = runtime_config_request_of(layout->application, layout->portable);
    request.plugin = prepared->id;
    request.config = cJSON_GetObjectItemCaseSensitive(prepared->manifest, "config");
    return runtime_config_settings_file(&request, &prepared->settings_file, refusal);
}

static spisor_status read_install(const runtime_testing_install *install, runtime_testing_prepared_install *prepared) {
    spisor_status status = record_json(install->manifest, 0, &prepared->manifest);
    if (status == SPISOR_OK) {
        status = record_json(install->settings, 1, &prepared->settings);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    prepared->id = text_member(prepared->manifest, "id");
    prepared->version = text_member(prepared->manifest, "version");
    prepared->entry_source = install->entry_source;
    if (prepared->id == NULL || prepared->version == NULL || text_member(prepared->manifest, "entry") == NULL ||
        !is_entry_source(install->entry_source)) {
        return SPISOR_MALFORMED;
    }
    return SPISOR_OK;
}

static spisor_status prepare_install(const runtime_testing_install *install, const runtime_testing_layout *layout,
                                     runtime_testing_prepared_install *prepared, spisor_refusal *refusal) {
    spisor_status status = read_install(install, prepared);
    if (status == SPISOR_OK) {
        status = place_bundle(prepared, layout->cache, text_member(prepared->manifest, "entry"));
    }
    if (status == SPISOR_MALFORMED) {
        return refuse(refusal, "an install is malformed or its bundle or entry climbs out of its folder", INSTALL_FIX);
    }
    return status == SPISOR_OK ? place_settings_file(prepared, layout, refusal) : status;
}

static spisor_status prepare_installs(const runtime_testing_home_argument *argument,
                                      runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    spisor_status status = SPISOR_OK;
    if (argument->install_count == 0) {
        return SPISOR_OK;
    }
    if (argument->installs == NULL) {
        return refuse(refusal, "the installs are absent beside a count", INSTALL_FIX);
    }
    home->installs = spisor_memory_allocate(argument->install_count * sizeof *home->installs);
    if (home->installs == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    memset(home->installs, 0, argument->install_count * sizeof *home->installs);
    home->install_count = argument->install_count;
    for (size_t index = 0; status == SPISOR_OK && index < argument->install_count; index++) {
        status = prepare_install(&argument->installs[index], &home->layout, &home->installs[index], refusal);
    }
    return status;
}

/** The version the library.json in directory declares, freed with cJSON_Delete through *declaration. */
static const char *declared_version(const char *directory, cJSON **declaration, int *out_of_memory) {
    const char *segments[2] = {directory, RUNTIME_LIBRARY_DECLARATION};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    size_t length = 0;
    uint8_t *bytes = path == NULL ? NULL : spisor_read_file_reporting(path, &length, &outcome);
    *out_of_memory = path == NULL || outcome == SPISOR_READ_OUT_OF_MEMORY;
    *declaration = bytes == NULL ? NULL : spisor_parse_json_reporting(bytes, length, out_of_memory);
    spisor_memory_release(bytes);
    spisor_memory_release(path);
    return text_member(*declaration, "version");
}

static spisor_status add_library(cJSON *libraries, const char *id, const char *version, const char *directory,
                                 const char *binary) {
    cJSON *entry = cJSON_AddObjectToObject(libraries, id);
    int added = entry != NULL && cJSON_AddStringToObject(entry, "version", version) != NULL &&
                cJSON_AddStringToObject(entry, "directory", directory) != NULL &&
                (binary == NULL || cJSON_AddStringToObject(entry, "binary", binary) != NULL);
    return added ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
}

static spisor_status prepare_library(const runtime_boot_library *library, cJSON *libraries) {
    cJSON *declaration = NULL;
    int out_of_memory = 0;
    const char *version = library->version;
    spisor_status status;
    if (library->id == NULL || library->id[0] == '\0' || library->directory == NULL ||
        !spisor_path_is_absolute(RUNTIME_HOST_IS_WINDOWS, library->directory) ||
        cJSON_GetObjectItemCaseSensitive(libraries, library->id) != NULL) {
        return SPISOR_MALFORMED;
    }
    if (version == NULL) {
        version = declared_version(library->directory, &declaration, &out_of_memory);
    }
    status = out_of_memory  ? SPISOR_OUT_OF_MEMORY
             : version == NULL ? SPISOR_MALFORMED
                               : add_library(libraries, library->id, version, library->directory, library->binary);
    cJSON_Delete(declaration);
    return status;
}

static spisor_status prepare_libraries(const runtime_testing_home_argument *argument,
                                       runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    spisor_status status = SPISOR_OK;
    if (argument->library_count > 0 && argument->libraries == NULL) {
        return refuse(refusal, "the libraries are absent beside a count", LIBRARY_FIX);
    }
    home->libraries = cJSON_CreateObject();
    if (home->libraries == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    for (size_t index = 0; status == SPISOR_OK && index < argument->library_count; index++) {
        status = prepare_library(&argument->libraries[index], home->libraries);
    }
    if (status == SPISOR_MALFORMED) {
        return refuse(refusal, "a library has no id or no absolute directory, is named twice, or has no version",
                      LIBRARY_FIX);
    }
    home->hints_default_koffi = cJSON_GetObjectItemCaseSensitive(home->libraries, KOFFI_ID) == NULL;
    if (status == SPISOR_OK && home->hints_default_koffi) {
        status = add_library(home->libraries, KOFFI_ID, DEFAULT_KOFFI_VERSION, home->layout.koffi, NULL);
    }
    return status;
}

static spisor_status check_host(const runtime_testing_home_argument *argument, spisor_refusal *refusal) {
    if (!runtime_registry_is_host_id(argument->host_id)) {
        return refuse(refusal, "the host id is absent or no plain file name", "pass a host id such as test-host");
    }
    if (argument->holder < 0) {
        return refuse(refusal, "the holder is negative", "pass the pid of the holding process, or 0 for none");
    }
    return SPISOR_OK;
}

spisor_status runtime_testing_prepare_home(const runtime_testing_home_argument *argument,
                                           runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    spisor_status status = check_base(argument->base, refusal);
    memset(home, 0, sizeof *home);
    if (status == SPISOR_OK) {
        status = check_host(argument, refusal);
    }
    if (status != SPISOR_OK) {
        return status;
    }
    home->base = argument->base;
    home->host_id = argument->host_id;
    home->holder = argument->holder;
    if (!runtime_testing_layout_of(argument->base, &home->layout)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = prepare_installs(argument, home, refusal);
    return status == SPISOR_OK ? prepare_libraries(argument, home, refusal) : status;
}

void runtime_testing_free_prepared_home(runtime_testing_prepared_home *home) {
    for (size_t index = 0; index < home->install_count; index++) {
        runtime_testing_prepared_install *install = &home->installs[index];
        cJSON_Delete(install->manifest);
        cJSON_Delete(install->settings);
        spisor_memory_release(install->bundle);
        spisor_memory_release(install->entry);
        spisor_memory_release(install->settings_file);
    }
    spisor_memory_release(home->installs);
    cJSON_Delete(home->libraries);
    runtime_testing_free_layout(&home->layout);
    memset(home, 0, sizeof *home);
}
