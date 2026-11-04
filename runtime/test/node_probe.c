#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define GRANDCHILD_SECONDS 30

static int executable_path(char *path, size_t size, const char *argument) {
#if defined(_WIN32)
    (void)argument;
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)size);
    return length != 0 && length < size;
#else
    if (strlen(argument) >= size) {
        return 0;
    }
    strcpy(path, argument);
    return 1;
#endif
}

/** Starts this program again as a grandchild that inherits standard output and outlives its parent. */
static int leave_grandchild(const char *self) {
#if defined(_WIN32)
    char line[4200];
    snprintf(line, sizeof line, "\"%s\" --linger", self);
    STARTUPINFOA startup;
    PROCESS_INFORMATION information;
    memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &information)) {
        return 4;
    }
    CloseHandle(information.hThread);
    CloseHandle(information.hProcess);
#else
    pid_t child = fork();
    if (child == 0) {
        execl(self, self, "--linger", (char *)NULL);
        _exit(127);
    }
    if (child < 0) {
        return 4;
    }
#endif
    printf("left\n");
    return 0;
}

static int linger(void) {
#if defined(_WIN32)
    Sleep(GRANDCHILD_SECONDS * 1000);
#else
    sleep(GRANDCHILD_SECONDS);
#endif
    return 0;
}

/**
 * Stands in for node on a PATH: answers any arguments with the contents of version.txt beside the
 * executable, as node -p process.versions.node answers its version. With --leave-grandchild it starts a
 * copy of itself that holds its standard output for half a minute and exits at once, as a node shim can.
 */
int main(int argc, char **argv) {
    char path[4096];
    if (argc < 1 || !executable_path(path, sizeof path, argv[0])) {
        return 2;
    }
    if (argc > 1 && strcmp(argv[1], "--leave-grandchild") == 0) {
        return leave_grandchild(path);
    }
    if (argc > 1 && strcmp(argv[1], "--linger") == 0) {
        return linger();
    }
    char *separator = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    separator = backslash != NULL && (separator == NULL || backslash > separator) ? backslash : separator;
    if (separator == NULL || (size_t)(separator - path) + sizeof "/version.txt" > sizeof path) {
        return 2;
    }
    strcpy(separator + 1, "version.txt");
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 3;
    }
    char version[64];
    size_t read = fread(version, 1, sizeof version - 1, file);
    fclose(file);
    version[read] = 0;
    printf("%s\n", version);
    return 0;
}
