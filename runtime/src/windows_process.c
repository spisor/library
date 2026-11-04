#include "windows_process.h"

#include <stdlib.h>
#include <string.h>

#include "cache_refusal.h"
#include "command_line_windows.h"

wchar_t *runtime_windows_text(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = units <= 0 ? NULL : malloc((size_t)units * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, units) != units) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

wchar_t *runtime_windows_command_line(const char *executable, const cJSON *arguments) {
    wchar_t *program = runtime_windows_text(executable);
    size_t size = program == NULL ? 0 : wcslen(program) + 3;
    for (const cJSON *argument = arguments == NULL ? NULL : arguments->child; argument != NULL; argument = argument->next) {
        size += 2 * strlen(argument->valuestring) + 3;
    }
    wchar_t *line = program == NULL ? NULL : malloc(size * sizeof *line);
    size_t used = 0;
    if (line != NULL) {
        line[used++] = L'"';
        wcscpy(line + used, program);
        used += wcslen(program);
        line[used++] = L'"';
    }
    for (const cJSON *argument = line == NULL || arguments == NULL ? NULL : arguments->child; argument != NULL;
         argument = argument->next) {
        wchar_t *wide = runtime_windows_text(argument->valuestring);
        if (wide == NULL) {
            free(line);
            line = NULL;
            break;
        }
        used = runtime_append_quoted_argument(line, used, wide);
        free(wide);
    }
    if (line != NULL) {
        line[used] = L'\0';
    }
    free(program);
    return line;
}

static int compare_entries(const void *left, const void *right) {
    const wchar_t *first = *(const wchar_t *const *)left;
    const wchar_t *second = *(const wchar_t *const *)right;
    int first_name = (int)(wcschr(first, L'=') - first);
    int second_name = (int)(wcschr(second, L'=') - second);
    return CompareStringOrdinal(first, first_name, second, second_name, TRUE) - CSTR_EQUAL;
}

static wchar_t *entry_of(const cJSON *variable) {
    const char *parts[3] = {variable->string, "=", variable->valuestring};
    char *text = runtime_cache_text(parts, 3);
    wchar_t *entry = text == NULL ? NULL : runtime_windows_text(text);
    free(text);
    return entry;
}

static wchar_t *joined_block(wchar_t **entries, size_t count) {
    size_t units = 2;
    for (size_t index = 0; index < count; index++) {
        units += wcslen(entries[index]) + 1;
    }
    wchar_t *block = malloc(units * sizeof *block);
    size_t used = 0;
    for (size_t index = 0; block != NULL && index < count; index++) {
        wcscpy(block + used, entries[index]);
        used += wcslen(entries[index]) + 1;
    }
    if (block != NULL) {
        block[used++] = L'\0';
        block[used] = L'\0';
    }
    return block;
}

wchar_t *runtime_windows_environment_block(const cJSON *environment) {
    size_t count = 0;
    for (const cJSON *variable = environment->child; variable != NULL; variable = variable->next) {
        count += cJSON_IsString(variable) ? 1 : 0;
    }
    wchar_t **entries = calloc(count + 1, sizeof *entries);
    int built = entries != NULL;
    size_t index = 0;
    for (const cJSON *variable = environment->child; built && variable != NULL; variable = variable->next) {
        if (cJSON_IsString(variable)) {
            entries[index] = entry_of(variable);
            built = entries[index++] != NULL;
        }
    }
    wchar_t *block = NULL;
    if (built) {
        qsort(entries, count, sizeof *entries, compare_entries);
        block = joined_block(entries, count);
    }
    for (size_t freed = 0; entries != NULL && freed < count; freed++) {
        free(entries[freed]);
    }
    free(entries);
    return block;
}

HANDLE runtime_windows_null_device(void) {
    SECURITY_ATTRIBUTES inherited = {sizeof inherited, NULL, TRUE};
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
                       OPEN_EXISTING, 0, NULL);
}

LPPROC_THREAD_ATTRIBUTE_LIST runtime_windows_handle_list(HANDLE *handles, DWORD count) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = malloc(size);
    if (attributes == NULL || !InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        free(attributes);
        return NULL;
    }
    if (count > 0 && !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                                count * sizeof handles[0], NULL, NULL)) {
        runtime_windows_free_handle_list(attributes);
        return NULL;
    }
    return attributes;
}

void runtime_windows_free_handle_list(LPPROC_THREAD_ATTRIBUTE_LIST attributes) {
    if (attributes != NULL) {
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
}
