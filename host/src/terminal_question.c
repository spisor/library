#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "terminal_question.h"

#include <stdlib.h>
#include <string.h>

#define LONGEST_ANSWER 1024

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"

typedef HANDLE terminal_file;
#define NO_TERMINAL_FILE INVALID_HANDLE_VALUE

static terminal_file open_terminal_file(const char *path, int for_writing) {
    wchar_t *wide = spisor_wide_path(path);
    terminal_file file = wide == NULL ? NO_TERMINAL_FILE
                                      : CreateFileW(wide, for_writing ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(wide);
    return file;
}

static int write_all(terminal_file file, const char *text) {
    DWORD written = 0;
    return WriteFile(file, text, (DWORD)strlen(text), &written, NULL) && written == strlen(text);
}

static int read_byte(terminal_file file, char *byte) {
    DWORD read = 0;
    return ReadFile(file, byte, 1, &read, NULL) && read == 1;
}

static void close_terminal_file(terminal_file file) {
    CloseHandle(file);
}

host_terminal host_controlling_terminal(void) {
    host_terminal terminal = {"\\\\.\\CONIN$", "\\\\.\\CONOUT$"};
    return terminal;
}

#else
#include <fcntl.h>
#include <unistd.h>

typedef int terminal_file;
#define NO_TERMINAL_FILE (-1)

static terminal_file open_terminal_file(const char *path, int for_writing) {
    return open(path, (for_writing ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOCTTY);
}

static int write_all(terminal_file file, const char *text) {
    size_t length = strlen(text);
    return write(file, text, length) == (ssize_t)length;
}

static int read_byte(terminal_file file, char *byte) {
    return read(file, byte, 1) == 1;
}

static void close_terminal_file(terminal_file file) {
    close(file);
}

host_terminal host_controlling_terminal(void) {
    host_terminal terminal = {"/dev/tty", "/dev/tty"};
    return terminal;
}

#endif

static int is_space(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f' ||
           character == '\v';
}

static char *trimmed(const char *line, size_t length) {
    size_t start = 0;
    while (start < length && is_space(line[start])) {
        start++;
    }
    while (length > start && is_space(line[length - 1])) {
        length--;
    }
    char *answer = malloc(length - start + 1);
    if (answer != NULL) {
        memcpy(answer, line + start, length - start);
        answer[length - start] = 0;
    }
    return answer;
}

/** The first line read from input, the last one unended included, without its ending; NULL when input holds none. */
static char *read_line(terminal_file input) {
    char line[LONGEST_ANSWER];
    size_t length = 0;
    char byte = 0;
    int ended = 0;
    while (!ended && read_byte(input, &byte)) {
        ended = byte == '\n';
        if (!ended && length < sizeof line) {
            line[length++] = byte;
        }
    }
    return ended || length > 0 ? trimmed(line, length) : NULL;
}

char *host_ask_on_terminal(const char *question, const host_terminal *terminal) {
    terminal_file input = open_terminal_file(terminal->input, 0);
    if (input == NO_TERMINAL_FILE) {
        return NULL;
    }
    terminal_file output = open_terminal_file(terminal->output, 1);
    char *answer = output != NO_TERMINAL_FILE && write_all(output, question) ? read_line(input) : NULL;
    if (output != NO_TERMINAL_FILE) {
        close_terminal_file(output);
    }
    close_terminal_file(input);
    return answer;
}
