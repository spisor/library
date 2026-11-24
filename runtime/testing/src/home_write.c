#include "home_write.h"

#include <string.h>

#include "boot_hint.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "catalog.h"
#include "definition_support.h"
#include "directory_tree.h"
#include "holders.h"
#include "install_entries.h"
#include "install_settings.h"
#include "paths.h"
#include "platform_key.h"
#include "registry.h"
#include "sha256.h"
#include "spisor/allocation.h"
#include "spisor/definition.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define REGISTERED_HOST_VERSION "1.0.0"
#define HINTED_HOST_VERSION "2.0.0"
#define PROVENANCE_METHOD "testing"
#define PROVENANCE_INSTALLED_AT "2026-01-01T00:00:00Z"
#define WRITE_FIX "check that the base is a folder this process can write"

static spisor_status refuse_write(spisor_refusal *refusal, const char *detail) {
    return spisor_refuse_as(refusal, SPISOR_REFUSED, detail, WRITE_FIX);
}

/** A write's answer as a status: OUT_OF_MEMORY passes, any other failure is refused naming `what`. */
static spisor_status written(spisor_status status, const char *what, spisor_refusal *refusal) {
    return status == SPISOR_OK || status == SPISOR_OUT_OF_MEMORY ? status : refuse_write(refusal, what);
}

/** A writer's problem text as a status: none is memory running out; it is freed. */
static spisor_status refuse_problem(char *problem, spisor_refusal *refusal) {
    spisor_status status = problem == NULL ? SPISOR_OUT_OF_MEMORY : refuse_write(refusal, problem);
    spisor_memory_release(problem);
    return status;
}

static spisor_status write_bytes(const char *path, const uint8_t *bytes, size_t length) {
    char *parent = runtime_cache_resolve(path, (const char *const[]){".."}, 1);
    spisor_status status = parent == NULL ? SPISOR_OUT_OF_MEMORY : spisor_make_directories(parent);
    spisor_memory_release(parent);
    return status == SPISOR_OK ? spisor_replace_file(path, bytes, length) : status;
}

static spisor_status write_json(const char *path, const cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    spisor_status status = text == NULL ? SPISOR_OUT_OF_MEMORY : write_bytes(path, (const uint8_t *)text, strlen(text));
    cJSON_free(text);
    return status;
}

static spisor_status write_catalog(const runtime_testing_layout *layout, spisor_refusal *refusal) {
    cJSON *catalog = cJSON_CreateObject();
    spisor_status status = catalog != NULL &&
                                   cJSON_AddNumberToObject(catalog, "format", RUNTIME_CATALOG_FORMAT) != NULL &&
                                   cJSON_AddObjectToObject(catalog, "items") != NULL
                               ? write_json(layout->catalog, catalog)
                               : SPISOR_OUT_OF_MEMORY;
    cJSON_Delete(catalog);
    return written(status, "the catalog index cannot be written", refusal);
}

static int add_platform(cJSON *provenance) {
    const char *key = runtime_platform_key();
    const char *hyphen = strchr(key, '-');
    char os[64];
    size_t length = hyphen == NULL ? 0 : (size_t)(hyphen - key);
    if (length == 0 || length >= sizeof os) {
        return 0;
    }
    memcpy(os, key, length);
    os[length] = '\0';
    return cJSON_AddStringToObject(provenance, "os", os) != NULL &&
           cJSON_AddStringToObject(provenance, "architecture", hyphen + 1) != NULL;
}

static cJSON *provenance_of(const runtime_testing_layout *layout) {
    cJSON *provenance = cJSON_CreateObject();
    if (provenance == NULL || cJSON_AddStringToObject(provenance, "method", PROVENANCE_METHOD) == NULL ||
        cJSON_AddStringToObject(provenance, "version", REGISTERED_HOST_VERSION) == NULL ||
        cJSON_AddStringToObject(provenance, "location", layout->application) == NULL || !add_platform(provenance) ||
        cJSON_AddStringToObject(provenance, "installedAt", PROVENANCE_INSTALLED_AT) == NULL) {
        cJSON_Delete(provenance);
        return NULL;
    }
    return provenance;
}

static cJSON *registration_of(const runtime_testing_prepared_home *home) {
    cJSON *registration = cJSON_CreateObject();
    cJSON *provenance = provenance_of(&home->layout);
    if (registration == NULL || provenance == NULL || cJSON_AddStringToObject(registration, "id", home->host_id) == NULL ||
        cJSON_AddStringToObject(registration, "kind", "host") == NULL ||
        cJSON_AddStringToObject(registration, "version", REGISTERED_HOST_VERSION) == NULL ||
        cJSON_AddStringToObject(registration, "home", home->layout.application) == NULL ||
        cJSON_AddFalseToObject(registration, "requiresCommand") == NULL ||
        !cJSON_AddItemToObject(registration, "provenance", provenance)) {
        cJSON_Delete(provenance);
        cJSON_Delete(registration);
        return NULL;
    }
    return registration;
}

static spisor_status register_host(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    cJSON *registration = registration_of(home);
    cJSON *operation_refusal = NULL;
    cJSON *registered;
    spisor_status status = written(spisor_make_directories(home->layout.application),
                                   "the host's application home cannot be made", refusal);
    if (status != SPISOR_OK || registration == NULL) {
        cJSON_Delete(registration);
        return status != SPISOR_OK ? status : SPISOR_OUT_OF_MEMORY;
    }
    registered = runtime_registry_register(home->layout.state, registration, &operation_refusal);
    cJSON_Delete(registration);
    if (registered == NULL) {
        return runtime_refuse_json(refusal, operation_refusal);
    }
    cJSON_Delete(registered);
    return SPISOR_OK;
}

static spisor_status hold_host(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    char *problem = NULL;
    char *path;
    runtime_holders_record record = runtime_no_holders();
    spisor_status status;
    if (home->holder == 0) {
        return SPISOR_OK;
    }
    path = runtime_holders_path(home->layout.state, home->host_id, ".json", &problem);
    if (path == NULL) {
        return refuse_problem(problem, refusal);
    }
    status = runtime_add_holder(&record, (double)home->holder) ? runtime_write_holders(path, &record)
                                                                : SPISOR_OUT_OF_MEMORY;
    runtime_free_holders(&record);
    spisor_memory_release(path);
    return written(status, "the host's processes record cannot be written", refusal);
}

static spisor_status hint_libraries(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    char *problem = NULL;
    if (home->hints_default_koffi) {
        spisor_status made = written(spisor_make_directories(home->layout.koffi), "koffi's folder cannot be made",
                                     refusal);
        if (made != SPISOR_OK) {
            return made;
        }
    }
    if (runtime_write_boot_hint(home->layout.cache, RUNTIME_JAVASCRIPT_HOST_ID, home->libraries, HINTED_HOST_VERSION,
                                &problem)) {
        return SPISOR_OK;
    }
    return refuse_problem(problem, refusal);
}

static spisor_status copy_file(const char *from, const char *to) {
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    size_t length = 0;
    uint8_t *bytes = spisor_read_file_reporting(from, &length, &outcome);
    spisor_status status = bytes != NULL                           ? write_bytes(to, bytes, length)
                           : outcome == SPISOR_READ_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY
                                                                   : SPISOR_REFUSED;
    spisor_memory_release(bytes);
    return status;
}

/** Records the file at path under key in hashes and sizes, as an install records what it stored. */
static spisor_status record_file(const char *path, const char *key, cJSON *hashes, cJSON *sizes) {
    char hex[RUNTIME_SHA256_HEX_SIZE];
    uint64_t size = 0;
    spisor_status status = runtime_sha256_file_sized(path, hex, &size);
    if (status != SPISOR_OK) {
        return status;
    }
    return cJSON_AddStringToObject(hashes, key, hex) != NULL && cJSON_AddNumberToObject(sizes, key, (double)size) != NULL
               ? SPISOR_OK
               : SPISOR_OUT_OF_MEMORY;
}

static spisor_status mark_installed(const runtime_testing_prepared_install *install, const char *manifest_path) {
    const char *entry = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(install->manifest, "entry"));
    cJSON *hashes = cJSON_CreateObject();
    cJSON *sizes = cJSON_CreateObject();
    spisor_status status = hashes == NULL || sizes == NULL ? SPISOR_OUT_OF_MEMORY
                                                           : record_file(manifest_path, RUNTIME_PLUGIN_MANIFEST, hashes,
                                                                         sizes);
    if (status == SPISOR_OK) {
        status = record_file(install->entry, entry, hashes, sizes);
    }
    if (status == SPISOR_OK) {
        status = runtime_cache_write_marker(install->bundle, hashes, sizes);
    }
    cJSON_Delete(sizes);
    cJSON_Delete(hashes);
    return status;
}

static spisor_status place_bundle(const runtime_testing_prepared_install *install) {
    const char *segments[2] = {install->bundle, RUNTIME_PLUGIN_MANIFEST};
    char *manifest_path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    spisor_status status = manifest_path == NULL ? SPISOR_OUT_OF_MEMORY : write_json(manifest_path, install->manifest);
    if (status == SPISOR_OK) {
        status = copy_file(install->entry_source, install->entry);
    }
    if (status == SPISOR_OK) {
        status = mark_installed(install, manifest_path);
    }
    spisor_memory_release(manifest_path);
    return status;
}

static spisor_status place_install(const runtime_testing_prepared_install *install, spisor_refusal *refusal) {
    spisor_status status = written(place_bundle(install), "a plugin's bundle cannot be written", refusal);
    if (status == SPISOR_OK && install->settings != NULL) {
        status = written(write_json(install->settings_file, install->settings),
                         "a plugin's settings file cannot be written", refusal);
    }
    return status;
}

static runtime_install_settings settings_of(const runtime_testing_layout *layout) {
    runtime_install_settings settings;
    settings.home = layout->application;
    settings.portable = layout->portable;
    settings.state = layout->state;
    return settings;
}

static cJSON *installed_ids(const runtime_testing_prepared_home *home) {
    cJSON *ids = cJSON_CreateArray();
    for (size_t index = 0; ids != NULL && index < home->install_count; index++) {
        if (!cJSON_AddItemToArray(ids, cJSON_CreateString(home->installs[index].id))) {
            cJSON_Delete(ids);
            ids = NULL;
        }
    }
    return ids;
}

static spisor_status enable_installs(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    runtime_install_settings settings = settings_of(&home->layout);
    cJSON *operation_refusal = NULL;
    cJSON *ids;
    int enabled;
    if (home->install_count == 0) {
        return SPISOR_OK;
    }
    ids = installed_ids(home);
    if (ids == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    enabled = runtime_install_enable_plugins(&settings, ids, &operation_refusal);
    cJSON_Delete(ids);
    return enabled ? SPISOR_OK : runtime_refuse_json(refusal, operation_refusal);
}

static cJSON *local_sources(const runtime_testing_layout *layout) {
    cJSON *sources = cJSON_CreateArray();
    cJSON *source = cJSON_CreateObject();
    if (sources == NULL || source == NULL || cJSON_AddStringToObject(source, "kind", "local") == NULL ||
        cJSON_AddStringToObject(source, "path", layout->catalog) == NULL || !cJSON_AddItemToArray(sources, source)) {
        cJSON_Delete(source);
        cJSON_Delete(sources);
        return NULL;
    }
    return sources;
}

static spisor_status record_catalog_source(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    runtime_install_settings settings = settings_of(&home->layout);
    cJSON *operation_refusal = NULL;
    cJSON *sources = local_sources(&home->layout);
    int recorded;
    if (sources == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    recorded = runtime_install_record_sources(&settings, sources, &operation_refusal);
    cJSON_Delete(sources);
    return recorded ? SPISOR_OK : runtime_refuse_json(refusal, operation_refusal);
}

spisor_status runtime_testing_write_home(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    spisor_status status = write_catalog(&home->layout, refusal);
    if (status == SPISOR_OK) {
        status = register_host(home, refusal);
    }
    if (status == SPISOR_OK) {
        status = hold_host(home, refusal);
    }
    if (status == SPISOR_OK) {
        status = hint_libraries(home, refusal);
    }
    for (size_t index = 0; status == SPISOR_OK && index < home->install_count; index++) {
        status = place_install(&home->installs[index], refusal);
    }
    if (status == SPISOR_OK) {
        status = enable_installs(home, refusal);
    }
    return status == SPISOR_OK ? record_catalog_source(home, refusal) : status;
}

spisor_status runtime_testing_empty_base(const char *base) {
    runtime_directory_listing listing = {NULL, 0};
    spisor_status status = runtime_list_directory(base, &listing);
    for (size_t index = 0; status == SPISOR_OK && index < listing.count; index++) {
        const char *segments[2] = {base, listing.entries[index].name};
        char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
        status = path == NULL ? SPISOR_OUT_OF_MEMORY : runtime_remove_tree(path);
        spisor_memory_release(path);
    }
    runtime_free_directory_listing(&listing);
    return status;
}
