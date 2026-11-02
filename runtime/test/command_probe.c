/**
 * The program commands_test points a shim at: command_probe <output> <exit code> <argument>... writes
 * each argument after the first two to output as one UTF-8 line and exits with the exit code.
 */
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>

static int write_line(FILE *output, const wchar_t *argument) {
    char text[4096];
    int size = WideCharToMultiByte(CP_UTF8, 0, argument, -1, text, (int)sizeof text, NULL, NULL);
    return size > 0 && fprintf(output, "%s\n", text) > 0;
}

int wmain(int count, wchar_t **arguments) {
    if (count < 3) {
        return 64;
    }
    FILE *output = _wfopen(arguments[1], L"wb");
    if (output == NULL) {
        return 65;
    }
    for (int index = 3; index < count; index++) {
        write_line(output, arguments[index]);
    }
    fclose(output);
    return _wtoi(arguments[2]);
}
#else
int main(int count, char **arguments) {
    if (count < 3) {
        return 64;
    }
    FILE *output = fopen(arguments[1], "wb");
    if (output == NULL) {
        return 65;
    }
    for (int index = 3; index < count; index++) {
        fprintf(output, "%s\n", arguments[index]);
    }
    fclose(output);
    return atoi(arguments[2]);
}
#endif
