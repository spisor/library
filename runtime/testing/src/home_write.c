#include "home_write.h"

#include <string.h>

#include "boot_hint.h"
#include "cache_folders.h"
#include "cache_marker.h"
#include "cache_place.h"
#include "catalog.h"
#include "catalog_source.h"
#include "config.h"
#include "definition_support.h"
#include "directory_tree.h"
#include "holders.h"
#include "hosts.h"
#include "install_entries.h"
#include "install_settings.h"
#include "paths.h"
#include "runtime/secrets.h"
#include "sha256_file.h"
#include "spisor/allocation.h"
#include "spisor/definition.h"
#include "spisor/file.h"
#include "spisor/path.h"

#define REGISTERED_HOST_VERSION "1.0.0"
#define HINTED_HOST_VERSION "2.0.0"
#define PROVENANCE_METHOD "testing"
#define INSTALLED_AT_MILLISECONDS 1767225600000.0
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
    const char *const up[1] = {".."};
    char *parent = runtime_cache_resolve(path, up, 1);
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

/** What the host's start would tell hosts' register: who it is, at the home it runs in, with no command. */
static cJSON *identity_of(const runtime_testing_prepared_home *home) {
    cJSON *identity = cJSON_CreateObject();
    if (identity == NULL || cJSON_AddStringToObject(identity, "id", home->host_id) == NULL ||
        cJSON_AddStringToObject(identity, "version", REGISTERED_HOST_VERSION) == NULL ||
        cJSON_AddStringToObject(identity, "home", home->layout.application) == NULL ||
        cJSON_AddFalseToObject(identity, "requiresCommand") == NULL ||
        cJSON_AddArrayToObject(identity, "commands") == NULL) {
        cJSON_Delete(identity);
        return NULL;
    }
    return identity;
}

/** The provenance hosts' provenance answers for a host installed by this library at a fixed time. */
static cJSON *provenance_of(const runtime_testing_prepared_home *home, const cJSON *environment, cJSON **refusal) {
    cJSON *origin = cJSON_CreateObject();
    cJSON *provenance = NULL;
    runtime_hosts_provenance_request request = {origin, home->host_id, home->layout.application, environment, 1,
                                                INSTALLED_AT_MILLISECONDS};
    *refusal = NULL;
    if (origin != NULL && cJSON_AddStringToObject(origin, "method", PROVENANCE_METHOD) != NULL &&
        cJSON_AddStringToObject(origin, "version", REGISTERED_HOST_VERSION) != NULL) {
        provenance = runtime_hosts_provenance(&request, refusal);
    }
    cJSON_Delete(origin);
    return provenance;
}

/** Registers the host through hosts' register, as its first start does. */
static spisor_status register_host(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    cJSON *environment = runtime_testing_layout_environment(&home->layout);
    cJSON *identity = identity_of(home);
    cJSON *operation_refusal = NULL;
    cJSON *provenance = environment == NULL ? NULL : provenance_of(home, environment, &operation_refusal);
    runtime_hosts_registration_request request = {home->layout.state, identity, provenance, environment};
    cJSON *registered = identity == NULL || provenance == NULL ? NULL : runtime_hosts_register(&request,
                                                                                               &operation_refusal);
    spisor_status status = registered != NULL ? SPISOR_OK : runtime_refuse_json(refusal, operation_refusal);
    cJSON_Delete(registered);
    cJSON_Delete(provenance);
    cJSON_Delete(identity);
    cJSON_Delete(environment);
    return status;
}

static spisor_status hold_host(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    char *problem = NULL;
    char *path;
    runtime_holders_record record = runtime_no_holders();
    spisor_status status;
    if (home->holder == 0) {
        return SPISOR_OK;
    }
    path = runtime_holders_path(home->layout.state, home->host_id, RUNTIME_JSON_EXTENSION, &problem);
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
    char hex[NAULA_SHA256_HEX_SIZE];
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
    if (status == SPISOR_OK && install->entry != NULL) {
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
    if (status == SPISOR_OK && install->entry != NULL) {
        status = copy_file(install->entry_source, install->entry);
    }
    if (status == SPISOR_OK) {
        status = mark_installed(install, manifest_path);
    }
    spisor_memory_release(manifest_path);
    return status;
}

/** Sets each of the install's settings through config's set, as the plugin's own set would. */
static spisor_status set_settings(const runtime_testing_layout *layout, const runtime_testing_prepared_install *install,
                                  spisor_refusal *refusal) {
    runtime_secret_places places = {sizeof(runtime_secret_places), layout->secrets_file,
                                    (uint32_t)layout->force_file_store};
    runtime_config_request request = runtime_config_request_of(layout->application, layout->portable);
    spisor_status status = SPISOR_OK;
    request.plugin = install->id;
    request.config = cJSON_GetObjectItemCaseSensitive(install->manifest, "config");
    request.settings = cJSON_GetObjectItemCaseSensitive(install->manifest, "settings");
    request.places = &places;
    for (const cJSON *setting = install->settings == NULL ? NULL : install->settings->child;
         status == SPISOR_OK && setting != NULL; setting = setting->next) {
        cJSON *previous = NULL;
        int secret = 0;
        request.key = setting->string;
        request.value = setting;
        status = runtime_config_set_value(&request, &previous, &secret, refusal);
        cJSON_Delete(previous);
    }
    return status;
}

spisor_status runtime_testing_place_install(const runtime_testing_layout *layout,
                                            const runtime_testing_prepared_install *install, spisor_refusal *refusal) {
    spisor_status status = written(place_bundle(install), "a plugin's bundle cannot be written", refusal);
    return status == SPISOR_OK ? set_settings(layout, install, refusal) : status;
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

static spisor_status record_catalog_source(const runtime_testing_prepared_home *home, spisor_refusal *refusal) {
    runtime_install_settings settings = settings_of(&home->layout);
    runtime_catalog_location location = {1, home->layout.catalog};
    cJSON *operation_refusal = NULL;
    cJSON *sources = cJSON_CreateArray();
    cJSON *source = runtime_catalog_source_json(&location);
    int recorded;
    if (sources == NULL || source == NULL || !cJSON_AddItemToArray(sources, source)) {
        cJSON_Delete(source);
        cJSON_Delete(sources);
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
        status = runtime_testing_place_install(&home->layout, &home->installs[index], refusal);
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
