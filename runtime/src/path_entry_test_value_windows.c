/**
 * Names the test build's stand-in for the user's Path once per test process, before main, so a process the test
 * starts has the name before either of them touches the value.
 *
 * @implNote A process that inherits no name owns a new one, SPISOR_TEST_PATH_<process id>, passes it on in
 * SPISOR_TEST_PATH_VALUE and deletes the value when it exits, whether its checks passed or not, so two test runs on
 * one machine never meet. Only a name under that prefix is inherited, so a stray variable cannot aim a test at Path.
 */
#include "path_entry_windows_hooks.h"

#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

#define VALUE_VARIABLE L"SPISOR_TEST_PATH_VALUE"
#define VALUE_PREFIX L"SPISOR_TEST_PATH_"
#define VALUE_PREFIX_LENGTH (sizeof VALUE_PREFIX / sizeof(wchar_t) - 1)
#define VALUE_CAPACITY 64

static wchar_t value_name[VALUE_CAPACITY];

const wchar_t *runtime_path_entry_test_value(void) {
    return value_name;
}

static int is_test_value_name(const wchar_t *name) {
    return wcsncmp(name, VALUE_PREFIX, VALUE_PREFIX_LENGTH) == 0 && name[VALUE_PREFIX_LENGTH] != L'\0';
}

static int inherit_value_name(void) {
    DWORD length = GetEnvironmentVariableW(VALUE_VARIABLE, value_name, VALUE_CAPACITY);
    int inherited = length > 0 && length < VALUE_CAPACITY && is_test_value_name(value_name);
    if (!inherited) {
        value_name[0] = L'\0';
    }
    return inherited;
}

static HKEY open_environment(void) {
    HKEY key = NULL;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    return status == ERROR_SUCCESS ? key : NULL;
}

static void delete_owned_value(void) {
    HKEY key = open_environment();
    if (key != NULL) {
        RegDeleteValueW(key, value_name);
        RegCloseKey(key);
    }
}

static int is_running(DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == NULL) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    int running = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

static int is_abandoned(const wchar_t *name) {
    if (!is_test_value_name(name)) {
        return 0;
    }
    wchar_t *end = NULL;
    unsigned long process_id = wcstoul(name + VALUE_PREFIX_LENGTH, &end, 10);
    return *end == L'\0' && !is_running((DWORD)process_id);
}

/** @implNote A test killed on its timeout never reaches its exit handler, so the next test run deletes its value. */
static void delete_abandoned_values(void) {
    HKEY key = open_environment();
    if (key == NULL) {
        return;
    }
    wchar_t name[VALUE_CAPACITY];
    DWORD index = 0;
    LSTATUS status = ERROR_SUCCESS;
    while (status != ERROR_NO_MORE_ITEMS) {
        DWORD length = VALUE_CAPACITY;
        status = RegEnumValueW(key, index, name, &length, NULL, NULL, NULL, NULL);
        int deleted = status == ERROR_SUCCESS && is_abandoned(name) && RegDeleteValueW(key, name) == ERROR_SUCCESS;
        index += deleted ? 0 : 1;
    }
    RegCloseKey(key);
}

/** @implNote _wputenv_s, not SetEnvironmentVariableW, so a child started through the C runtime inherits it too. */
static void own_value_name(void) {
    delete_abandoned_values();
    swprintf(value_name, VALUE_CAPACITY, VALUE_PREFIX L"%lu", (unsigned long)GetCurrentProcessId());
    _wputenv_s(VALUE_VARIABLE, value_name);
    atexit(delete_owned_value);
}

static void name_test_value(void) {
    if (!inherit_value_name()) {
        own_value_name();
    }
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*const runtime_path_entry_test_value_naming)(void) = name_test_value;
#else
__attribute__((constructor, used)) static void name_test_value_at_start(void) {
    name_test_value();
}
#endif
