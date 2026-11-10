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
    "  forebay-host install --name <[marketplace/]name> --application <id> [--accept <id>] [--accept-primary <id>]\n"                                            \
    "  forebay-host status\n"                                                                                         \
    "  forebay-host update\n"                                                                                         \
    "  forebay-host remove <host-id>\n"                                                                               \
    "  forebay-host launch <loader-id> [arguments for the application]...\n"                                          \
    "  forebay-host background <host-id>\n"                                                                           \
    "  forebay-host hook <loader-id> <event>\n"                                                                       \
    "  forebay-host catalog build [--plugin <directory>]... [--host <directory>[@<platform>]]... "                    \
    "[--library <id>=<directory>@<platform>]... --out <directory>\n"                                                  \
    "  forebay-host marketplace build --list <path> --out <path>\n"

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


char *host_printable(const char *text) {
    size_t length = strlen(text);
    char *escaped = malloc(length * 6 + 1);
    size_t used = 0;
    for (size_t index = 0; escaped != NULL && index < length; index++) {
        unsigned char byte = (unsigned char)text[index];
        int is_c1 = byte == 0xC2 && index + 1 < length && (unsigned char)text[index + 1] >= 0x80 &&
                    (unsigned char)text[index + 1] <= 0x9F;
        if (is_c1) {
            used += (size_t)snprintf(escaped + used, 7, "\\u00%02x", (unsigned)(unsigned char)text[++index]);
        } else if (byte < 0x20 || byte == 0x7F) {
            used += (size_t)snprintf(escaped + used, 5, "\\x%02x", (unsigned)byte);
        } else {
            escaped[used++] = (char)byte;
        }
    }
    if (escaped != NULL) {
        escaped[used] = 0;
    }
    return escaped;
}

void host_print_refusal(const cJSON *refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(refusal, "fix");
    host_write(1, cJSON_IsString(message) ? message->valuestring : "forebay-host ran out of memory");
    host_write(1, "\nfix: ");
    host_write(1, cJSON_IsString(fix) ? fix->valuestring : "close other programs, then run the command again");
    host_write(1, "\n");
}

void host_print_refusal_printably(const cJSON *refusal) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(refusal, "message");
    const cJSON *fix = cJSON_GetObjectItemCaseSensitive(refusal, "fix");
    char *safe_message = cJSON_IsString(message) ? host_printable(message->valuestring) : NULL;
    char *safe_fix = cJSON_IsString(fix) ? host_printable(fix->valuestring) : NULL;
    host_write(1, safe_message == NULL ? "forebay-host ran out of memory" : safe_message);
    host_write(1, "\nfix: ");
    host_write(1, safe_fix == NULL ? "close other programs, then run the command again" : safe_fix);
    host_write(1, "\n");
    free(safe_fix);
    free(safe_message);
}

int host_refuse_printably(cJSON *refusal) {
    host_print_refusal_printably(refusal);
    cJSON_Delete(refusal);
    return HOST_EXIT_REFUSED;
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
