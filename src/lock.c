#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#endif

#include "spisor/lock.h"
#include "allocation.h"

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

static spisor_status taken_or_refused(int locked) {
    return locked ? SPISOR_OK : SPISOR_REFUSED;
}

#if defined(_WIN32)

static spisor_status open_lock_file(const char *path, HANDLE *handle) {
    *handle = INVALID_HANDLE_VALUE;
    if (path == NULL || !spisor_is_convertible_path(path)) {
        return SPISOR_REFUSED;
    }
    wchar_t *wide = spisor_wide_path(path);
    if (wide == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    *handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    spisor_memory_release(wide);
    return *handle == INVALID_HANDLE_VALUE ? SPISOR_REFUSED : SPISOR_OK;
}

spisor_status spisor_lock_file(const char *path, spisor_lock **lock) {
    HANDLE handle;
    *lock = NULL;
    spisor_status status = open_lock_file(path, &handle);
    if (status != SPISOR_OK) {
        return status;
    }
    OVERLAPPED whole = {0};
    spisor_lock *taken = spisor_memory_allocate(sizeof *taken);
    status = taken == NULL ? SPISOR_OUT_OF_MEMORY
                           : taken_or_refused(LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &whole));
    if (status != SPISOR_OK) {
        spisor_memory_release(taken);
        CloseHandle(handle);
        return status;
    }
    taken->handle = handle;
    *lock = taken;
    return SPISOR_OK;
}

void spisor_unlock(spisor_lock *lock) {
    if (lock == NULL) {
        return;
    }
    OVERLAPPED whole = {0};
    UnlockFileEx(lock->handle, 0, 1, 0, &whole);
    CloseHandle(lock->handle);
    spisor_memory_release(lock);
}

#else

static int lock_exclusively(int descriptor) {
    int locked;
    do {
        locked = flock(descriptor, LOCK_EX) == 0;
    } while (!locked && errno == EINTR);
    return locked;
}

spisor_status spisor_lock_file(const char *path, spisor_lock **lock) {
    *lock = NULL;
    int descriptor = path == NULL ? -1 : open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return SPISOR_REFUSED;
    }
    spisor_lock *taken = spisor_memory_allocate(sizeof *taken);
    spisor_status status = taken == NULL ? SPISOR_OUT_OF_MEMORY : taken_or_refused(lock_exclusively(descriptor));
    if (status != SPISOR_OK) {
        spisor_memory_release(taken);
        close(descriptor);
        return status;
    }
    taken->descriptor = descriptor;
    *lock = taken;
    return SPISOR_OK;
}

void spisor_unlock(spisor_lock *lock) {
    if (lock == NULL) {
        return;
    }
    flock(lock->descriptor, LOCK_UN);
    close(lock->descriptor);
    spisor_memory_release(lock);
}

#endif
