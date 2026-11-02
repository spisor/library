/**
 * The Windows command shim: <state>/bin/<name>.exe is a copy of this program, and <name>.shim beside
 * it holds the UTF-8 path of the program it runs. It runs that program with its own arguments, passed
 * on exactly as they were given, and exits with the program's exit code.
 *
 * @implNote A .cmd shim cannot do this: cmd.exe decodes a batch file in the console's OEM code page,
 * so a target under a user folder with a non-ASCII letter is garbled at run time. Everything here is UTF-16 from the
 * command line to CreateProcessW, and the target file is UTF-8 by construction.
 */
#include <windows.h>

#define LAUNCH_FAILED 127

static void complain(const char *message) {
    DWORD written = 0;
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error != NULL && error != INVALID_HANDLE_VALUE) {
        WriteFile(error, message, (DWORD)lstrlenA(message), &written, NULL);
    }
}

static wchar_t *own_path(void) {
    DWORD capacity = MAX_PATH;
    for (int attempt = 0; attempt < 8; attempt++) {
        wchar_t *path = HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(wchar_t));
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
    wchar_t *path = HeapAlloc(GetProcessHeap(), 0, (length + 6) * sizeof(wchar_t));
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
        bytes = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    }
    if (bytes != NULL && (!ReadFile(file, bytes, (DWORD)size.QuadPart, length, NULL) || *length == 0)) {
        HeapFree(GetProcessHeap(), 0, bytes);
        bytes = NULL;
    }
    CloseHandle(file);
    return bytes;
}

static wchar_t *read_target(const wchar_t *shim) {
    DWORD length = 0;
    char *bytes = read_bytes(shim, &length);
    while (bytes != NULL && length > 0 && (bytes[length - 1] == '\r' || bytes[length - 1] == '\n')) {
        length--;
    }
    int units = bytes == NULL || length == 0
                    ? 0
                    : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length, NULL, 0);
    wchar_t *target = units > 0 ? HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)units + 1) * sizeof(wchar_t)) : NULL;
    if (target != NULL) {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length, target, units);
        target[units] = L'\0';
    }
    if (bytes != NULL) {
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    return target;
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

static wchar_t *child_command_line(const wchar_t *target, const wchar_t *arguments) {
    size_t size = (size_t)lstrlenW(target) + (size_t)lstrlenW(arguments) + 4;
    wchar_t *line = HeapAlloc(GetProcessHeap(), 0, size * sizeof(wchar_t));
    if (line == NULL) {
        return NULL;
    }
    lstrcpyW(line, L"\"");
    lstrcatW(line, target);
    lstrcatW(line, L"\"");
    if (*arguments != L'\0') {
        lstrcatW(line, L" ");
        lstrcatW(line, arguments);
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
    wchar_t *target = shim == NULL ? NULL : read_target(shim);
    if (target == NULL) {
        complain("spisor: this command's .shim file is missing or unreadable\n");
        return LAUNCH_FAILED;
    }
    wchar_t *line = child_command_line(target, arguments_after_program(GetCommandLineW()));
    return line == NULL ? LAUNCH_FAILED : run(line);
}
