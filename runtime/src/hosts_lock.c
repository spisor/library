#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif

#include "hosts_lock.h"

#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

struct runtime_hosts_guard {
#if defined(_WIN32)
    HANDLE handle;
#else
    int descriptor;
#endif
};

static char *lock_path(const char *state) {
    const char *directory_segments[2] = {state, "hosts"};
    char *directory = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, directory_segments, 2);
    if (directory == NULL || !spisor_make_directories(directory)) {
        free(directory);
        return NULL;
    }
    const char *segments[2] = {directory, ".lock"};
    char *path = spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
    free(directory);
    return path;
}

#if defined(_WIN32)

static HANDLE open_lock_file(const char *path) {
    wchar_t *wide = spisor_wide_path(path);
    HANDLE handle = INVALID_HANDLE_VALUE;
    if (wide != NULL) {
        handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    free(wide);
    return handle;
}

runtime_hosts_guard *runtime_hosts_lock(const char *state) {
    char *path = lock_path(state);
    HANDLE handle = path == NULL ? INVALID_HANDLE_VALUE : open_lock_file(path);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    OVERLAPPED whole = {0};
    runtime_hosts_guard *guard = malloc(sizeof *guard);
    if (guard == NULL || !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole)) {
        free(guard);
        CloseHandle(handle);
        return NULL;
    }
    guard->handle = handle;
    return guard;
}

void runtime_hosts_unlock(runtime_hosts_guard *guard) {
    if (guard == NULL) {
        return;
    }
    OVERLAPPED whole = {0};
    UnlockFileEx(guard->handle, 0, 1, 0, &whole);
    CloseHandle(guard->handle);
    free(guard);
}

#else

static int lock_exclusively(int descriptor) {
    int locked;
    do {
        locked = flock(descriptor, LOCK_EX) == 0;
    } while (!locked && errno == EINTR);
    return locked;
}

runtime_hosts_guard *runtime_hosts_lock(const char *state) {
    char *path = lock_path(state);
    int descriptor = path == NULL ? -1 : open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(path);
    if (descriptor < 0) {
        return NULL;
    }
    runtime_hosts_guard *guard = malloc(sizeof *guard);
    if (guard == NULL || !lock_exclusively(descriptor)) {
        free(guard);
        close(descriptor);
        return NULL;
    }
    guard->descriptor = descriptor;
    return guard;
}

void runtime_hosts_unlock(runtime_hosts_guard *guard) {
    if (guard == NULL) {
        return;
    }
    flock(guard->descriptor, LOCK_UN);
    close(guard->descriptor);
    free(guard);
}

#endif
