#include "scope_host.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "allocation.h"
#include "paths.h"

#ifdef _WIN32
static SRWLOCK host_lock = SRWLOCK_INIT;

static void lock_host(void) {
    AcquireSRWLockExclusive(&host_lock);
}

static void unlock_host(void) {
    ReleaseSRWLockExclusive(&host_lock);
}
#else
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

static void lock_host(void) {
    pthread_mutex_lock(&host_lock);
}

static void unlock_host(void) {
    pthread_mutex_unlock(&host_lock);
}
#endif

/** The started host, guarded by host_lock; `home` NULL while none is started. */
static runtime_owned_scope_host started;

void runtime_scope_host_free(runtime_owned_scope_host *copy) {
    spisor_memory_release(copy->home);
    spisor_memory_release(copy->portable);
    cJSON_Delete(copy->names);
    memset(copy, 0, sizeof *copy);
}

/** A copy of `host` into an empty `copy`; 0, with `copy` left empty, when memory runs out. */
static int copy_host(const char *home, const cJSON *names, const char *portable, runtime_owned_scope_host *copy) {
    copy->home = spisor_memory_duplicate_text(home);
    copy->portable = portable == NULL ? NULL : spisor_memory_duplicate_text(portable);
    copy->names = names == NULL ? NULL : cJSON_Duplicate(names, 1);
    if (copy->home == NULL || (portable != NULL && copy->portable == NULL) || (names != NULL && copy->names == NULL)) {
        runtime_scope_host_free(copy);
        return 0;
    }
    return 1;
}

static int is_usable_host(const runtime_scope_host *host) {
    return host != NULL && runtime_is_usable_home(host->home) &&
           (host->portable == NULL || runtime_is_usable_home(host->portable)) &&
           (host->names == NULL || cJSON_IsObject(host->names));
}

spisor_status runtime_scopes_begin(const runtime_scope_host *host) {
    runtime_owned_scope_host fresh = {NULL, NULL, NULL};
    runtime_owned_scope_host replaced;
    if (!is_usable_host(host)) {
        return SPISOR_MALFORMED;
    }
    if (!copy_host(host->home, host->names, host->portable, &fresh)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    lock_host();
    replaced = started;
    started = fresh;
    unlock_host();
    runtime_scope_host_free(&replaced);
    return SPISOR_OK;
}

void runtime_scopes_end(void) {
    runtime_owned_scope_host ended;
    lock_host();
    ended = started;
    memset(&started, 0, sizeof started);
    unlock_host();
    runtime_scope_host_free(&ended);
}

spisor_status runtime_scopes_host(runtime_owned_scope_host *copy) {
    spisor_status status = SPISOR_NONE;
    memset(copy, 0, sizeof *copy);
    lock_host();
    if (started.home != NULL) {
        status = copy_host(started.home, started.names, started.portable, copy) ? SPISOR_OK : SPISOR_OUT_OF_MEMORY;
    }
    unlock_host();
    return status;
}
