/**
 * The Windows command shim: <state>/bin/<name>.exe is a copy of this program, and <name>.shim beside
 * it holds, in the format shim_format.h names, the program it runs and the fixed arguments it always
 * passes. It runs that program with the fixed arguments and then its own, passed on exactly as they
 * were given, and exits with the program's exit code.
 *
 * @implNote A .cmd shim cannot do this: cmd.exe decodes a batch file in the console's OEM code page,
 * so a target under a user folder with a non-ASCII letter is garbled at run time. Everything here is UTF-16 from the
 * command line to CreateProcessW, and the .shim file is UTF-8 by construction.
 */
#include <windows.h>

#include "command_line_windows.h"
#include "shim_format.h"

#define LAUNCH_FAILED 127
#define FIRST_ARGUMENT_LINE 2

typedef struct shim_lines {
    wchar_t **items;
    int count;
} shim_lines;

typedef enum shim_reading { SHIM_READ, SHIM_UNREADABLE, SHIM_UNKNOWN_FORMAT } shim_reading;

static void complain(const char *message) {
    DWORD written = 0;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error != NULL && error != INVALID_HANDLE_VALUE) {
        WriteFile(error, message, (DWORD)lstrlenA(message), &written, NULL);
    }
}

static void *allocate(SIZE_T size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

static wchar_t *own_path(void) {
    DWORD capacity = MAX_PATH;
    for (int attempt = 0; attempt < 8; attempt++) {
        wchar_t *path = allocate(capacity * sizeof(wchar_t));
        if (path == NULL) {
            return NULL;
        }
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity) {
            return path;
        }
        HeapFree(GetProcessHeap(), 0, path);
        capacity *= 2;
    }
    return NULL;
}

/** own with the extension of its file name replaced by .shim, freed by the process exiting. */
static wchar_t *shim_path_of(const wchar_t *own) {
    size_t length = (size_t)lstrlenW(own);
    wchar_t *path = allocate((length + 6) * sizeof(wchar_t));
    if (path == NULL) {
        return NULL;
    }
    lstrcpyW(path, own);
    wchar_t *dot = NULL;
    for (wchar_t *cursor = path; *cursor != L'\0'; cursor++) {
        dot = *cursor == L'.' ? cursor : (*cursor == L'\\' || *cursor == L'/') ? NULL : dot;
    }
    lstrcpyW(dot == NULL ? path + length : dot, L".shim");
    return path;
}

static char *read_bytes(const wchar_t *path, DWORD *length) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    LARGE_INTEGER size;
    char *bytes = NULL;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 65536) {
        bytes = allocate((SIZE_T)size.QuadPart);
    }
    if (bytes != NULL && (!ReadFile(file, bytes, (DWORD)size.QuadPart, length, NULL) || *length == 0)) {
        HeapFree(GetProcessHeap(), 0, bytes);
        bytes = NULL;
    }
    CloseHandle(file);
    return bytes;
}

/** The UTF-8 bytes of shim as UTF-16, NUL-terminated, with their length in units; NULL when unreadable. */
static wchar_t *read_text(const wchar_t *shim, int *units) {
    DWORD length = 0;
    char *bytes = read_bytes(shim, &length);
    *units = bytes == NULL ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length, NULL, 0);
    wchar_t *text = *units > 0 ? allocate(((SIZE_T)*units + 1) * sizeof(wchar_t)) : NULL;
    if (text != NULL) {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length, text, *units);
        text[*units] = L'\0';
    }
    if (bytes != NULL) {
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    return text;
}

/** Cuts text into its lines in place, a \r before each \n dropped; 0 when it does not end in \n. */
static int split_lines(wchar_t *text, int units, shim_lines *lines) {
    if (text[units - 1] != L'\n') {
        return 0;
    }
    lines->count = 0;
    for (int index = 0; index < units; index++) {
        lines->count += text[index] == L'\n';
    }
    lines->items = allocate((SIZE_T)lines->count * sizeof(wchar_t *));
    if (lines->items == NULL) {
        return 0;
    }
    wchar_t *start = text;
    int line = 0;
    for (int index = 0; index < units; index++) {
        if (text[index] == L'\n') {
            text[index] = L'\0';
            if (index > 0 && text[index - 1] == L'\r') {
                text[index - 1] = L'\0';
            }
            lines->items[line++] = start;
            start = text + index + 1;
        }
    }
    return 1;
}

static int is_known_format(const shim_lines *lines) {
    return lines->count >= FIRST_ARGUMENT_LINE && lstrcmpW(lines->items[0], L"" RUNTIME_SHIM_FORMAT) == 0 &&
           lines->items[1][0] != L'\0';
}

static shim_reading read_shim(const wchar_t *shim, shim_lines *lines) {
    int units = 0;
    wchar_t *text = read_text(shim, &units);
    if (text == NULL) {
        return SHIM_UNREADABLE;
    }
    return split_lines(text, units, lines) && is_known_format(lines) ? SHIM_READ : SHIM_UNKNOWN_FORMAT;
}

/**
 * What follows the program name on line, as CommandLineToArgvW reads the program name: up to the
 * closing quote when it starts with one, otherwise up to the first space or tab.
 */
static const wchar_t *arguments_after_program(const wchar_t *line) {
    const wchar_t *cursor = line;
    if (*cursor == L'"') {
        cursor++;
        while (*cursor != L'\0' && *cursor != L'"') {
            cursor++;
        }
        cursor += *cursor == L'"';
    } else {
        while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') {
            cursor++;
        }
    }
    while (*cursor == L' ' || *cursor == L'\t') {
        cursor++;
    }
    return cursor;
}

/** The target as the program, each fixed argument quoted, then the caller's arguments as they were given. */
static wchar_t *child_command_line(const shim_lines *lines, const wchar_t *callers) {
    const wchar_t *target = lines->items[1];
    size_t size = (size_t)lstrlenW(target) + (size_t)lstrlenW(callers) + 4;
    for (int index = FIRST_ARGUMENT_LINE; index < lines->count; index++) {
        size += 2 * (size_t)lstrlenW(lines->items[index]) + 3;
    }
    wchar_t *line = allocate(size * sizeof(wchar_t));
    if (line == NULL) {
        return NULL;
    }
    lstrcpyW(line, L"\"");
    lstrcatW(line, target);
    lstrcatW(line, L"\"");
    size_t used = (size_t)lstrlenW(line);
    for (int index = FIRST_ARGUMENT_LINE; index < lines->count; index++) {
        used = runtime_append_quoted_argument(line, used, lines->items[index]);
    }
    line[used] = L'\0';
    if (*callers != L'\0') {
        lstrcatW(line, L" ");
        lstrcatW(line, callers);
    }
    return line;
}

static BOOL WINAPI outlive_interrupt(DWORD event) {
    (void)event;
    return TRUE;
}

/**
 * @implNote Ctrl+C reaches every process on the console, so the launcher survives it with a handler
 * of its own while the program it runs decides what an interrupt does. A handler routine is not
 * inherited; SetConsoleCtrlHandler(NULL, TRUE) would be, and would leave the program unable to be
 * interrupted.
 */
static int run(wchar_t *line) {
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetConsoleCtrlHandler(outlive_interrupt, TRUE);
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        complain("spisor: the program this command runs could not be started\n");
        return LAUNCH_FAILED;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = LAUNCH_FAILED;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)code;
}

int wmain(void) {
    wchar_t *own = own_path();
    wchar_t *shim = own == NULL ? NULL : shim_path_of(own);
    shim_lines lines = {NULL, 0};
    shim_reading reading = shim == NULL ? SHIM_UNREADABLE : read_shim(shim, &lines);
    if (reading == SHIM_UNREADABLE) {
        complain("spisor: this command's .shim file is missing or unreadable\n");
        return LAUNCH_FAILED;
    }
    if (reading == SHIM_UNKNOWN_FORMAT) {
        complain("spisor: this command's .shim file is not in the format this launcher reads; let its host ask "
                 "for the command again\n");
        return LAUNCH_FAILED;
    }
    wchar_t *line = child_command_line(&lines, arguments_after_program(GetCommandLineW()));
    return line == NULL ? LAUNCH_FAILED : run(line);
}
