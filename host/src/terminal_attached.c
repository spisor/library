#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "terminal_question.h"

#if defined(HOST_TERMINAL_TEST_HOOK)
#include <stdlib.h>
#include <string.h>
#endif

#if defined(_WIN32)
#include <windows.h>

static int has_terminal(void) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

#else
#include <unistd.h>

static int has_terminal(void) {
    return isatty(0) && isatty(1);
}

#endif

int host_terminal_attached(void) {
#if defined(HOST_TERMINAL_TEST_HOOK)
    const char *forced = getenv("SPISOR_TEST_TERMINAL_ATTACHED");
    if (forced != NULL && strcmp(forced, "1") == 0) {
        return 1;
    }
#endif
    return has_terminal();
}
