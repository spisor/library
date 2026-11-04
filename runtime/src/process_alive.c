#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_alive.h"

#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#endif

#define LARGEST_PROCESS_ID 4294967295.0

static int is_process_id(double pid) {
    return pid > 0 && pid <= LARGEST_PROCESS_ID && floor(pid) == pid;
}

#if defined(_WIN32)

int runtime_process_is_alive(double pid) {
    if (!is_process_id(pid)) {
        return 0;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (process == NULL) {
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD exit_code = 0;
    int alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

#else

int runtime_process_is_alive(double pid) {
    if (!is_process_id(pid) || pid > 2147483647.0) {
        return 0;
    }
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

#endif
