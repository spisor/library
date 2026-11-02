#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "secret_backend.h"
#ifdef SPISOR_SECRET_TEST_HOOKS
#include "secret_linux_hooks.h"
#endif

/**
 * @implNote The declarations below mirror the libsecret 0.18 ABI (libsecret-1.so.0) and the glib, gobject
 * and gio functions it exports through its dependencies, so nothing is needed at build time. SecretSchema
 * is a name, a flags int and 32 name/type attribute pairs followed by reserved words; GError is a quark
 * domain, a code and a message; GList is data, next, previous. All have been stable since libsecret 0.16.
 */
typedef struct glib_error {
    uint32_t domain;
    int code;
    char *message;
} glib_error;

typedef struct glib_list {
    void *data;
    struct glib_list *next;
    struct glib_list *previous;
} glib_list;

typedef struct secret_schema_attribute {
    const char *name;
    int type;
} secret_schema_attribute;

typedef struct secret_schema {
    const char *name;
    int flags;
    secret_schema_attribute attributes[32];
    int reserved;
    void *reserved_pointers[7];
} secret_schema;

typedef unsigned (*hash_function)(const void *key);
typedef int (*equal_function)(const void *first, const void *second);
typedef void (*destroy_function)(void *data);

enum { SECRET_SCHEMA_ATTRIBUTE_STRING = 0, SECRET_SEARCH_ALL = 2, IO_ERROR_CANCELLED = 19 };
enum { SECRET_ERROR_PROTECTED = 1, SECRET_ERROR_IS_LOCKED = 2 };

typedef struct secret_api {
    void *handle;
    int (*store)(const secret_schema *, void *, const char *, const char *, const char *, void *, glib_error **);
    char *(*lookup)(const secret_schema *, void *, void *, glib_error **);
    int (*clear)(const secret_schema *, void *, void *, glib_error **);
    glib_list *(*search)(void *, const secret_schema *, void *, int, void *, glib_error **);
    void (*free_password)(char *);
    uint32_t (*secret_error_quark)(void);
    uint32_t (*io_error_quark)(void);
    uint32_t (*dbus_error_quark)(void);
    void *(*table_new)(hash_function, equal_function, destroy_function, destroy_function);
    int (*table_insert)(void *, void *, void *);
    void *(*table_lookup)(void *, const void *);
    void (*table_unref)(void *);
    unsigned (*string_hash)(const void *);
    int (*string_equal)(const void *, const void *);
    void (*error_free)(glib_error *);
    void (*list_free_full)(glib_list *, destroy_function);
    void (*object_unref)(void *);
    void *(*item_attributes)(void *);
} secret_api;

#define DEFAULT_LIBRARY_NAME "libsecret-1.so.0"
static const char *const entry_attribute = "entry";
static const char *const probe_entry = "spisor-availability-probe";

static const secret_schema spisor_schema = {"spisor", 0, {{"entry", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}, 0,
                                            {NULL, NULL, NULL, NULL, NULL, NULL, NULL}};

/**
 * The libsecret entry points, loaded on first use and kept for the life of the process.
 *
 * @implNote Every access goes through load_api, which holds api_lock, so any thread may call the
 * backend. Symbols are resolved from the libsecret handle alone: dlsym searches the library's own
 * dependencies, and glib, gobject and gio are all of them, so nothing else is opened by name. An
 * absent library is remembered too, so a machine without it pays one failed dlopen.
 */
static secret_api loaded_api;
static int load_attempted;
static const char *library_name_in_use = DEFAULT_LIBRARY_NAME;
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;

static int resolve(void *handle, const char *symbol, void *slot) {
    void *address = dlsym(handle, symbol);
    memcpy(slot, &address, sizeof address);
    return address != NULL;
}

static int resolve_secret_functions(secret_api *api) {
    void *handle = api->handle;
    return resolve(handle, "secret_password_storev_sync", &api->store) &&
           resolve(handle, "secret_password_lookupv_sync", &api->lookup) &&
           resolve(handle, "secret_password_clearv_sync", &api->clear) &&
           resolve(handle, "secret_service_search_sync", &api->search) &&
           resolve(handle, "secret_password_free", &api->free_password) &&
           resolve(handle, "secret_error_get_quark", &api->secret_error_quark) &&
           resolve(handle, "secret_item_get_attributes", &api->item_attributes);
}

static int resolve_glib_functions(secret_api *api) {
    void *handle = api->handle;
    return resolve(handle, "g_io_error_quark", &api->io_error_quark) &&
           resolve(handle, "g_dbus_error_quark", &api->dbus_error_quark) &&
           resolve(handle, "g_hash_table_new_full", &api->table_new) &&
           resolve(handle, "g_hash_table_insert", &api->table_insert) &&
           resolve(handle, "g_hash_table_lookup", &api->table_lookup) &&
           resolve(handle, "g_hash_table_unref", &api->table_unref) &&
           resolve(handle, "g_str_hash", &api->string_hash) && resolve(handle, "g_str_equal", &api->string_equal) &&
           resolve(handle, "g_error_free", &api->error_free) &&
           resolve(handle, "g_list_free_full", &api->list_free_full) &&
           resolve(handle, "g_object_unref", &api->object_unref);
}

static void discard_api(void) {
    if (loaded_api.handle != NULL) {
        dlclose(loaded_api.handle);
    }
    memset(&loaded_api, 0, sizeof loaded_api);
}

static const secret_api *load_api(void) {
    pthread_mutex_lock(&api_lock);
    if (!load_attempted) {
        load_attempted = 1;
        loaded_api.handle = dlopen(library_name_in_use, RTLD_NOW | RTLD_LOCAL);
        if (loaded_api.handle != NULL && !(resolve_secret_functions(&loaded_api) && resolve_glib_functions(&loaded_api))) {
            discard_api();
        }
    }
    const secret_api *api = loaded_api.handle == NULL ? NULL : &loaded_api;
    pthread_mutex_unlock(&api_lock);
    return api;
}

#ifdef SPISOR_SECRET_TEST_HOOKS
void runtime_secret_linux_use_library(const char *library_name) {
    pthread_mutex_lock(&api_lock);
    discard_api();
    load_attempted = 0;
    library_name_in_use = library_name == NULL ? DEFAULT_LIBRARY_NAME : library_name;
    pthread_mutex_unlock(&api_lock);
}

int runtime_secret_linux_library_is_loaded(void) {
    return load_api() != NULL;
}
#endif

static int environment_has(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0';
}

/**
 * Whether a session bus can be reached without starting one.
 *
 * @implNote Without this check, a machine that has libsecret but no session bus (a headless runner, a
 * container) sends GIO down its D-Bus autolaunch path, which runs dbus-launch and can stall or start a
 * stray daemon. The bus address variable, or the per-user socket systemd places at
 * $XDG_RUNTIME_DIR/bus, is what a real desktop session provides.
 */
static int session_bus_is_reachable(void) {
    if (environment_has("DBUS_SESSION_BUS_ADDRESS")) {
        return 1;
    }
    const char *runtime_directory = getenv("XDG_RUNTIME_DIR");
    char socket_path[4096];
    if (runtime_directory == NULL || runtime_directory[0] == '\0' ||
        strlen(runtime_directory) + sizeof "/bus" > sizeof socket_path) {
        return 0;
    }
    strcpy(socket_path, runtime_directory);
    strcat(socket_path, "/bus");
    return access(socket_path, F_OK) == 0;
}

static const secret_api *reachable_api(void) {
    return session_bus_is_reachable() ? load_api() : NULL;
}

/**
 * Maps a libsecret error to what the seam reports, and frees it.
 *
 * @implNote A dismissed unlock prompt is reported by libsecret as G_IO_ERROR_CANCELLED, and a collection
 * that stays locked as SECRET_ERROR_IS_LOCKED; both are the user saying no, so DECLINED. Any other GIO
 * or D-Bus error means the service could not be reached or does not exist (no owner for the bus name, no
 * bus), which is UNAVAILABLE so the secrets file takes over. Only an error inside the service itself is
 * FAILED.
 */
static runtime_secret_answer answer_for_error(const secret_api *api, glib_error *error) {
    runtime_secret_answer answer = RUNTIME_SECRET_FAILED;
    if (error->domain == api->io_error_quark() && error->code == IO_ERROR_CANCELLED) {
        answer = RUNTIME_SECRET_DECLINED;
    } else if (error->domain == api->secret_error_quark() &&
               (error->code == SECRET_ERROR_IS_LOCKED || error->code == SECRET_ERROR_PROTECTED)) {
        answer = RUNTIME_SECRET_DECLINED;
    } else if (error->domain == api->io_error_quark() || error->domain == api->dbus_error_quark()) {
        answer = RUNTIME_SECRET_UNAVAILABLE;
    }
    api->error_free(error);
    return answer;
}

static void *attributes_for(const secret_api *api, const char *entry) {
    void *table = api->table_new(api->string_hash, api->string_equal, NULL, NULL);
    if (table != NULL) {
        api->table_insert(table, (void *)entry_attribute, (void *)entry);
    }
    return table;
}

int runtime_secret_store_available(void) {
    const secret_api *api = reachable_api();
    void *attributes = api == NULL ? NULL : attributes_for(api, probe_entry);
    if (attributes == NULL) {
        return 0;
    }
    glib_error *error = NULL;
    char *found = api->lookup(&spisor_schema, attributes, NULL, &error);
    api->table_unref(attributes);
    if (found != NULL) {
        api->free_password(found);
    }
    return error == NULL || answer_for_error(api, error) != RUNTIME_SECRET_UNAVAILABLE;
}

runtime_secret_answer runtime_secret_store_get(const char *entry, char **value) {
    *value = NULL;
    const secret_api *api = reachable_api();
    if (api == NULL) {
        return RUNTIME_SECRET_UNAVAILABLE;
    }
    void *attributes = attributes_for(api, entry);
    if (attributes == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    glib_error *error = NULL;
    char *found = api->lookup(&spisor_schema, attributes, NULL, &error);
    api->table_unref(attributes);
    if (error != NULL) {
        return answer_for_error(api, error);
    }
    if (found == NULL) {
        return RUNTIME_SECRET_ABSENT;
    }
    *value = strdup(found);
    api->free_password(found);
    return *value == NULL ? RUNTIME_SECRET_FAILED : RUNTIME_SECRET_DONE;
}

runtime_secret_answer runtime_secret_store_set(const char *entry, const char *value) {
    const secret_api *api = reachable_api();
    if (api == NULL) {
        return RUNTIME_SECRET_UNAVAILABLE;
    }
    void *attributes = attributes_for(api, entry);
    if (attributes == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    glib_error *error = NULL;
    int stored = api->store(&spisor_schema, attributes, "default", entry, value, NULL, &error);
    api->table_unref(attributes);
    if (error != NULL) {
        return answer_for_error(api, error);
    }
    return stored ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_FAILED;
}

runtime_secret_answer runtime_secret_store_delete(const char *entry) {
    const secret_api *api = reachable_api();
    if (api == NULL) {
        return RUNTIME_SECRET_UNAVAILABLE;
    }
    void *attributes = attributes_for(api, entry);
    if (attributes == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    glib_error *error = NULL;
    int removed = api->clear(&spisor_schema, attributes, NULL, &error);
    api->table_unref(attributes);
    if (error != NULL) {
        return answer_for_error(api, error);
    }
    return removed ? RUNTIME_SECRET_DONE : RUNTIME_SECRET_ABSENT;
}

static int append_name(runtime_secret_names *names, size_t *capacity, const char *name) {
    if (names->count == *capacity) {
        size_t grown = *capacity == 0 ? 8 : *capacity * 2;
        char **resized = realloc(names->names, grown * sizeof *resized);
        if (resized == NULL) {
            return 0;
        }
        names->names = resized;
        *capacity = grown;
    }
    names->names[names->count] = strdup(name);
    if (names->names[names->count] == NULL) {
        return 0;
    }
    names->count++;
    return 1;
}

static int collect_matching_names(const secret_api *api, glib_list *items, const char *prefix,
                                  runtime_secret_names *names) {
    size_t capacity = 0;
    size_t prefix_length = strlen(prefix);
    for (glib_list *item = items; item != NULL; item = item->next) {
        void *attributes = api->item_attributes(item->data);
        const char *entry = attributes == NULL ? NULL : api->table_lookup(attributes, entry_attribute);
        int kept = entry == NULL || strncmp(entry, prefix, prefix_length) != 0 || append_name(names, &capacity, entry);
        if (attributes != NULL) {
            api->table_unref(attributes);
        }
        if (!kept) {
            return 0;
        }
    }
    return 1;
}

/**
 * @implNote Items are searched with SECRET_SEARCH_ALL only: no unlock and no secret loading, so listing
 * never prompts. The attribute table is empty so every item of the "spisor" schema comes back, and the
 * prefix is applied here.
 */
runtime_secret_answer runtime_secret_store_list(const char *prefix, runtime_secret_names *names) {
    names->names = NULL;
    names->count = 0;
    const secret_api *api = reachable_api();
    if (api == NULL) {
        return RUNTIME_SECRET_UNAVAILABLE;
    }
    void *attributes = api->table_new(api->string_hash, api->string_equal, NULL, NULL);
    if (attributes == NULL) {
        return RUNTIME_SECRET_FAILED;
    }
    glib_error *error = NULL;
    glib_list *items = api->search(NULL, &spisor_schema, attributes, SECRET_SEARCH_ALL, NULL, &error);
    api->table_unref(attributes);
    if (error != NULL) {
        return answer_for_error(api, error);
    }
    int collected = collect_matching_names(api, items, prefix, names);
    api->list_free_full(items, api->object_unref);
    if (!collected) {
        runtime_free_secret_names(names);
        return RUNTIME_SECRET_FAILED;
    }
    return RUNTIME_SECRET_DONE;
}
