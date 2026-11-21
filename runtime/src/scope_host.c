#include "scope_host.h"

#include <string.h>

#include "allocation.h"
#include "file_times.h"
#include "log_writer.h"
#include "paths.h"
#include "thread.h"

/** Serialises begin and end, so the writer one end stops is never the one a begin is starting. */
static runtime_mutex lifecycle_lock = RUNTIME_MUTEX_INIT;
static runtime_mutex host_lock = RUNTIME_MUTEX_INIT;

/** The started host, guarded by host_lock; `home` NULL while none is started. */
static runtime_owned_scope_host started;

void runtime_scope_host_free(runtime_owned_scope_host *copy) {
    spisor_memory_release(copy->home);
    spisor_memory_release(copy->portable);
    spisor_memory_release(copy->cache);
    spisor_memory_release(copy->secrets_file);
    cJSON_Delete(copy->names);
    memset(copy, 0, sizeof *copy);
}

static char *copy_optional(const char *text) {
    return text == NULL ? NULL : spisor_memory_duplicate_text(text);
}

/** A copy of `host` into an empty `copy`, `start` naming its session; 0, with `copy` left empty, out of memory. */
static int copy_host(const runtime_scope_host *host, const char *start, runtime_owned_scope_host *copy) {
    copy->home = spisor_memory_duplicate_text(host->home);
    copy->portable = copy_optional(host->portable);
    copy->cache = spisor_memory_duplicate_text(host->cache);
    copy->secrets_file = spisor_memory_duplicate_text(host->secrets_file);
    copy->names = host->names == NULL ? NULL : cJSON_Duplicate(host->names, 1);
    copy->force_file_store = host->force_file_store;
    copy->no_color = host->no_color;
    memcpy(copy->start, start, sizeof copy->start);
    if (copy->home == NULL || copy->cache == NULL || copy->secrets_file == NULL ||
        (host->portable != NULL && copy->portable == NULL) || (host->names != NULL && copy->names == NULL)) {
        runtime_scope_host_free(copy);
        return 0;
    }
    return 1;
}

static runtime_scope_host borrowed(const runtime_owned_scope_host *owned) {
    runtime_scope_host host;
    host.home = owned->home;
    host.names = owned->names;
    host.portable = owned->portable;
    host.cache = owned->cache;
    host.secrets_file = owned->secrets_file;
    host.force_file_store = owned->force_file_store;
    host.no_color = owned->no_color;
    return host;
}

static int is_usable_host(const runtime_scope_host *host) {
    return host != NULL && runtime_is_usable_home(host->home) &&
           (host->portable == NULL || runtime_is_usable_home(host->portable)) &&
           runtime_is_usable_home(host->cache) && runtime_is_usable_home(host->secrets_file) &&
           (host->names == NULL || cJSON_IsObject(host->names));
}

static spisor_status replace_started(const runtime_scope_host *host, const char *start) {
    runtime_owned_scope_host fresh;
    runtime_owned_scope_host replaced;
    memset(&fresh, 0, sizeof fresh);
    if (!copy_host(host, start, &fresh)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    runtime_mutex_lock(&host_lock);
    replaced = started;
    started = fresh;
    runtime_mutex_unlock(&host_lock);
    runtime_scope_host_free(&replaced);
    return SPISOR_OK;
}

spisor_status runtime_scopes_begin(const runtime_scope_host *host) {
    char start[RUNTIME_CATALOG_TIME_SIZE];
    spisor_status status;
    if (!is_usable_host(host)) {
        return SPISOR_MALFORMED;
    }
    if (!runtime_catalog_format_time(runtime_now_milliseconds(), start)) {
        return SPISOR_REFUSED;
    }
    runtime_mutex_lock(&lifecycle_lock);
    status = runtime_log_writer_start();
    if (status == SPISOR_OK) {
        status = replace_started(host, start);
    }
    runtime_mutex_unlock(&lifecycle_lock);
    return status;
}

void runtime_scopes_end(void) {
    runtime_owned_scope_host ended;
    runtime_mutex_lock(&lifecycle_lock);
    runtime_mutex_lock(&host_lock);
    ended = started;
    memset(&started, 0, sizeof started);
    runtime_mutex_unlock(&host_lock);
    runtime_log_writer_stop();
    runtime_mutex_unlock(&lifecycle_lock);
    runtime_scope_host_free(&ended);
}

spisor_status runtime_scopes_host(runtime_owned_scope_host *copy) {
    spisor_status status = SPISOR_NONE;
    runtime_scope_host host;
    memset(copy, 0, sizeof *copy);
    runtime_mutex_lock(&host_lock);
    if (started.home != NULL) {
        host = borrowed(&started);
        status = copy_host(&host, started.start, copy) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    }
    runtime_mutex_unlock(&host_lock);
    return status;
}
