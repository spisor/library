#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif

#include "spisor/lock.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

struct spisor_lock {
#if defined(_WIN32)
    HANDLE handle;
#else
    int descriptor;
#endif
};

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

spisor_lock *spisor_lock_file(const char *path) {
    HANDLE handle = path == NULL ? INVALID_HANDLE_VALUE : open_lock_file(path);
    if (handle == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    OVERLAPPED whole = {0};
    spisor_lock *lock = malloc(sizeof *lock);
    if (lock == NULL || !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole)) {
        free(lock);
        CloseHandle(handle);
        return NULL;
    }
    lock->handle = handle;
    return lock;
}

void spisor_unlock(spisor_lock *lock) {
    if (lock == NULL) {
        return;
    }
    OVERLAPPED whole = {0};
    UnlockFileEx(lock->handle, 0, 1, 0, &whole);
    CloseHandle(lock->handle);
    free(lock);
}

#else

static int lock_exclusively(int descriptor) {
    int locked;
    do {
        locked = flock(descriptor, LOCK_EX) == 0;
    } while (!locked && errno == EINTR);
    return locked;
}

spisor_lock *spisor_lock_file(const char *path) {
    int descriptor = path == NULL ? -1 : open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return NULL;
    }
    spisor_lock *lock = malloc(sizeof *lock);
    if (lock == NULL || !lock_exclusively(descriptor)) {
        free(lock);
        close(descriptor);
        return NULL;
    }
    lock->descriptor = descriptor;
    return lock;
}

void spisor_unlock(spisor_lock *lock) {
    if (lock == NULL) {
        return;
    }
    flock(lock->descriptor, LOCK_UN);
    close(lock->descriptor);
    free(lock);
}

#endif
