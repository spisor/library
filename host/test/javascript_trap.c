#include <stdio.h>

#if defined(_WIN32)
#include <wchar.h>
#endif

#if defined(_WIN32)

/** Stands as node or bun on a PATH: appends its arguments to the file SPISOR_JAVASCRIPT_TRAP names, so a test sees it ran. */
int wmain(int count, wchar_t **arguments) {
    const wchar_t *trap = _wgetenv(L"SPISOR_JAVASCRIPT_TRAP");
    FILE *file = trap == NULL ? NULL : _wfopen(trap, L"a");
    for (int index = 0; file != NULL && index < count; index++) {
        fwprintf(file, L"%ls%ls", index == 0 ? L"" : L" ", arguments[index]);
    }
    if (file != NULL) {
        fputws(L"\n", file);
        fclose(file);
    }
    return 0;
}

#else

#include <stdlib.h>

/** Stands as node or bun on a PATH: appends its arguments to the file SPISOR_JAVASCRIPT_TRAP names, so a test sees it ran. */
int main(int count, char **arguments) {
    const char *trap = getenv("SPISOR_JAVASCRIPT_TRAP");
    FILE *file = trap == NULL ? NULL : fopen(trap, "a");
    for (int index = 0; file != NULL && index < count; index++) {
        fprintf(file, "%s%s", index == 0 ? "" : " ", arguments[index]);
    }
    if (file != NULL) {
        fputs("\n", file);
        fclose(file);
    }
    return 0;
}

#endif
