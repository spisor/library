#ifndef SPISOR_MEMORY_ERROR_H
#define SPISOR_MEMORY_ERROR_H

#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/**
 * Whether a C library call that failed did so because memory ran out, by the errno it set.
 *
 * @implNote fopen, fdopen, opendir, realpath and dlopen allocate through the C library's own allocator, which the
 * binary's counting allocator never sees, so only their errno tells memory running out from any other failure.
 */
static inline int spisor_is_memory_errno(int error) {
    return error == ENOMEM;
}

#if defined(_WIN32)
/** Whether a Windows call that failed did so because memory ran out, by the error GetLastError answered. */
static inline int spisor_is_memory_windows_error(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY;
}
#endif

#endif
