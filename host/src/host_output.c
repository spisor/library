#include "host_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/json_write.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#define USAGE                                                                                                         \
    "usage:\n"                                                                                                        \
    "  forebay-host install --catalog <path or url> [--plugin <id>]... [--application <id>=<method|skip>]...\n"      \
    "  forebay-host status\n"                                                                                         \
    "  forebay-host update\n"                                                                                         \
    "  forebay-host remove <host-id>\n"                                                                               \
    "  forebay-host launch <loader-id> [arguments for the application]...\n"                                          \
    "  forebay-host background <host-id>\n"                                                                           \
    "  forebay-host hook <loader-id> <event>\n"                                                                       \
    "  forebay-host catalog build [--plugin <directory>]... [--host <directory>[@<platform>]]... "                    \
    "[--library <id>=<directory>@<platform>]... --out <directory>\n"

#if defined(_WIN32)

static int written_to_console(HANDLE stream, const char *text) {
    DWORD mode = 0;
    if (!GetConsoleMode(stream, &mode)) {
        return 0;
    }
    int units = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide = units <= 0 ? NULL : malloc((size_t)units * sizeof *wide);
    int written = wide != NULL && MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, units) == units &&
                  WriteConsoleW(stream, wide, (DWORD)(units - 1), NULL, NULL);
    free(wide);
    return written;
}

void host_write(int to_error, const char *text) {
    FILE *stream = to_error ? stderr : stdout;
    fflush(stream);
    if (!written_to_console(GetStdHandle(to_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE), text)) {
        fwrite(text, 1, strlen(text), stream);
        fflush(stream);
    }
}

#else

void host_write(int to_error, const char *text) {
    FILE *stream = to_error ? stderr : stdout;
    fwrite(text, 1, strlen(text), stream);
    fflush(stream);
}

#endif

void host_print_json(const cJSON *value) {
    char *text = spisor_json_stringify_pretty(value);
    if (text != NULL) {
        host_write(0, text);
        host_write(0, "\n");
    }
    free(text);
}

void host_print_refusal(const cJSON *refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(refusal, "fix");
    host_write(1, cJSON_IsString(message) ? message->valuestring : "forebay-host ran out of memory");
    host_write(1, "\nfix: ");
    host_write(1, cJSON_IsString(fix) ? fix->valuestring : "close other programs, then run the command again");
    host_write(1, "\n");
}

int host_refuse(cJSON *refusal) {
    host_print_refusal(refusal);
    cJSON_Delete(refusal);
    return HOST_EXIT_REFUSED;
}

int host_misuse(const char *message) {
    host_write(1, message);
    host_write(1, "\n" USAGE);
    return HOST_EXIT_MISUSED;
}
