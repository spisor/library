#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/**
 * Stands in for node on a PATH: answers any arguments with the contents of version.txt beside the
 * executable, as node -p process.versions.node answers its version.
 */
int main(int argc, char **argv) {
    char path[4096];
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    DWORD length = GetModuleFileNameA(NULL, path, sizeof path);
    if (length == 0 || length >= sizeof path) {
        return 2;
    }
#else
    if (argc < 1 || strlen(argv[0]) >= sizeof path) {
        return 2;
    }
    strcpy(path, argv[0]);
#endif
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
