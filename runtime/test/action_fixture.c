#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define make_folder(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_folder(path) mkdir(path, 0700)
#endif

/**
 * @file
 * A native language host's entry as child_start_test needs one, run as `<runtime binary> <language> <loader> action
 * <plugin> <action> <input> <result>`, making the result's folder as runtime's run does. It writes {consoleWindow,
 * mark, arguments} to the file SPISOR_FIXTURE_REPORT names, `mark` the value of SPISOR_FIXTURE_MARK, then runs the
 * action: `greet` writes "hello" to the result path and exits 0, as plugin-stub's greet answers; `fail` writes a line
 * to standard error and exits 3; `fail-silently` exits 4; `garbage` writes a result that is no JSON; `noisy` writes
 * NOISY_LENGTH bytes of standard error, a warning first, then a euro sign whose last two bytes start the final
 * RUNTIME_RUN_OUTPUT_LIMIT, then filler, an invalid byte, a NUL and its error last, and exits 5; `crash` exits as an
 * access violation does on Windows, 0xC0000005; any other writes nothing and exits 0.
 */

#define ACTION_ARGUMENTS 9
#define OUTPUT_LIMIT 4096
#define WARNING_LENGTH 1000
#define NOISY_ERROR "invalid \xFF here, nul \0 here, the action's own error"

/** The standard error `noisy` writes, through one fwrite so a NUL in it reaches the pipe. */
static void write_noise(void) {
    static char noise[WARNING_LENGTH + 1 + OUTPUT_LIMIT];
    size_t error_length = sizeof NOISY_ERROR - 1;
    memset(noise, 'w', WARNING_LENGTH);
    memcpy(noise + WARNING_LENGTH, "\xE2\x82\xAC", 3);
    memset(noise + WARNING_LENGTH + 3, 'y', OUTPUT_LIMIT - 2 - error_length);
    memcpy(noise + sizeof noise - error_length, NOISY_ERROR, error_length);
    fwrite(noise, 1, sizeof noise, stderr);
    fflush(stderr);
}

static int has_console_window(void) {
#if defined(_WIN32)
    return GetConsoleWindow() != NULL;
#else
    return 0;
#endif
}

static void write_escaped(FILE *file, const char *text) {
    fputc('"', file);
    for (; text != NULL && *text != '\0'; text++) {
        if (*text == '"' || *text == '\\') {
            fputc('\\', file);
        }
        fputc(*text, file);
    }
    fputc('"', file);
}

static void report(int count, char **arguments) {
    const char *path = getenv("SPISOR_FIXTURE_REPORT");
    FILE *file = path == NULL ? NULL : fopen(path, "wb");
    if (file == NULL) {
        return;
    }
    fprintf(file, "{\"consoleWindow\": %s, \"mark\": ", has_console_window() ? "true" : "false");
    write_escaped(file, getenv("SPISOR_FIXTURE_MARK"));
    fputs(", \"arguments\": [", file);
    for (int index = 1; index < count; index++) {
        fputs(index == 1 ? "" : ", ", file);
        write_escaped(file, arguments[index]);
    }
    fputs("]}", file);
    fclose(file);
}

/** Makes the folder path ends in, the results folder, as runtime_write_result makes it when missing. */
static void make_parent(const char *path) {
    char parent[4096];
    snprintf(parent, sizeof parent, "%s", path);
    char *last = strrchr(parent, '/');
    char *other = strrchr(parent, '\\');
    last = other != NULL && (last == NULL || other > last) ? other : last;
    if (last != NULL) {
        *last = '\0';
        make_folder(parent);
    }
}

static int write_result(const char *path, const char *text) {
    make_parent(path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 2;
    }
    fputs(text, file);
    return fclose(file) == 0 ? 0 : 2;
}

int main(int count, char **arguments) {
    report(count, arguments);
    if (count != ACTION_ARGUMENTS || strcmp(arguments[4], "action") != 0) {
        fputs("action-fixture: expected <runtime binary> <language> <loader> action <plugin> <action> <input> <result>\n",
              stderr);
        return 2;
    }
    const char *action = arguments[6];
    const char *result = arguments[8];
    fputs("written to standard output, which no caller reads\n", stdout);
    if (strcmp(action, "greet") == 0) {
        return write_result(result, "\"hello\"");
    }
    if (strcmp(action, "garbage") == 0) {
        return write_result(result, "not json");
    }
    if (strcmp(action, "fail") == 0) {
        fputs("  the fixture's action failed\n", stderr);
        return 3;
    }
    if (strcmp(action, "noisy") == 0) {
        write_noise();
        return 5;
    }
#if defined(_WIN32)
    if (strcmp(action, "crash") == 0) {
        ExitProcess(0xC0000005u);
    }
#endif
    return strcmp(action, "fail-silently") == 0 ? 4 : 0;
}
