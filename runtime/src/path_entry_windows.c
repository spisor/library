/**
 * The path-entry kind on Windows: the directory kept as one entry of the user's Path value under
 * HKCU\Environment, which is where Windows keeps a user's PATH for every process started afterwards.
 *
 * @implNote This file writes the user's environment, the one state runtime changes that no argument
 * names, and broadcasts WM_SETTINGCHANGE after each write so running shells read it again. A build
 * with SPISOR_PATH_ENTRY_TEST_HOOKS, which every test links and the library never does, writes
 * RUNTIME_PATH_ENTRY_TEST_VALUE instead of Path.
 */
#include "path_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#ifdef SPISOR_PATH_ENTRY_TEST_HOOKS
#include "path_entry_windows_hooks.h"
static const wchar_t *const path_value_name = RUNTIME_PATH_ENTRY_TEST_VALUE;
#else
static const wchar_t *const path_value_name = L"Path";
#endif

#define READABLE_TYPES (RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND)
#define BROADCAST_TIMEOUT_MILLISECONDS 5000

typedef struct path_value {
    wchar_t *text;
    DWORD type;
    int present;
} path_value;

typedef struct entry_span {
    size_t start;
    size_t end;
    int found;
} entry_span;

typedef int (*entry_matcher)(const wchar_t *entry, size_t length, const void *wanted);

static const char *text_of(const cJSON *object, const char *member) {
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, member));
}

static void write_note(runtime_change_note *note, const char *reason, const char *directory) {
    snprintf(note->detail, sizeof note->detail, "%s: %s", reason, directory);
}

static runtime_change_step fail(runtime_change_note *note, const char *reason, const char *directory) {
    write_note(note, reason, directory);
    return RUNTIME_CHANGE_FAILED;
}

static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = length > 0 ? malloc((size_t)length * sizeof(wchar_t)) : NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static char *to_narrow(const wchar_t *text, size_t length) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
    char *narrow = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, text, (int)length, narrow, size, NULL, NULL) <= 0) {
        free(narrow);
        return NULL;
    }
    if (narrow != NULL) {
        narrow[size] = '\0';
    }
    return narrow;
}

const char *runtime_path_entry_defect(const char *directory) {
    return strpbrk(directory, ";%\"\r\n") == NULL
               ? NULL
               : "a path-entry directory cannot hold ;, %, \" or a line break, which would split or expand its Path "
                 "entry";
}

runtime_change_step runtime_path_entry_children(const cJSON *change, cJSON **children, runtime_change_note *note) {
    (void)change;
    (void)note;
    *children = NULL;
    return RUNTIME_CHANGE_DONE;
}

static HKEY open_environment(void) {
    HKEY key = NULL;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                                     NULL, &key, NULL);
    return status == ERROR_SUCCESS ? key : NULL;
}

static int read_absent(path_value *value) {
    value->type = REG_EXPAND_SZ;
    value->present = 0;
    value->text = calloc(1, sizeof(wchar_t));
    return value->text != NULL;
}

/**
 * Reads the value as stored, never expanded, so a REG_EXPAND_SZ written back keeps its %VARIABLES%.
 * An absent value reads as empty, typed REG_EXPAND_SZ as Windows creates it. A value of any other
 * type is a failure, so it is never overwritten.
 *
 * @implNote The size is asked first and the read retried, since the value can grow between the two calls.
 */
static int read_path_value(HKEY key, path_value *value) {
    value->text = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        DWORD size = 0;
        LSTATUS status = RegGetValueW(key, NULL, path_value_name, READABLE_TYPES, &value->type, NULL, &size);
        if (status == ERROR_FILE_NOT_FOUND) {
            return read_absent(value);
        }
        wchar_t *text = status == ERROR_SUCCESS ? malloc(size + sizeof(wchar_t)) : NULL;
        if (text == NULL) {
            return 0;
        }
        status = RegGetValueW(key, NULL, path_value_name, READABLE_TYPES, &value->type, text, &size);
        if (status == ERROR_SUCCESS) {
            value->text = text;
            value->present = 1;
            return 1;
        }
        free(text);
        if (status != ERROR_MORE_DATA) {
            return 0;
        }
    }
    return 0;
}

static void announce_environment_change(void) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG,
                        BROADCAST_TIMEOUT_MILLISECONDS, &result);
}

static int store_path_text(HKEY key, DWORD type, const wchar_t *text) {
    DWORD size = (DWORD)((wcslen(text) + 1) * sizeof(wchar_t));
    int stored = RegSetValueExW(key, path_value_name, 0, type, (const BYTE *)text, size) == ERROR_SUCCESS;
    if (stored) {
        announce_environment_change();
    }
    return stored;
}

static int delete_path_value(HKEY key) {
    LSTATUS status = RegDeleteValueW(key, path_value_name);
    if (status == ERROR_SUCCESS) {
        announce_environment_change();
    }
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

static int is_same_directory(const wchar_t *entry, size_t length, const void *wanted) {
    char *narrow = to_narrow(entry, length);
    int same = narrow != NULL && runtime_change_paths_equal(narrow, (const char *)wanted);
    free(narrow);
    return same;
}

static int is_exact_entry(const wchar_t *entry, size_t length, const void *wanted) {
    const wchar_t *exact = wanted;
    return wcslen(exact) == length && wmemcmp(entry, exact, length) == 0;
}

static entry_span last_entry(const wchar_t *text, entry_matcher matches, const void *wanted) {
    entry_span span = {0, 0, 0};
    size_t length = wcslen(text);
    size_t start = 0;
    while (start <= length) {
        const wchar_t *separator = wcschr(text + start, L';');
        size_t end = separator == NULL ? length : (size_t)(separator - text);
        if (end > start && matches(text + start, end - start, wanted)) {
            span.start = start;
            span.end = end;
            span.found = 1;
        }
        start = end + 1;
    }
    return span;
}

static int holds_directory(const path_value *value, const char *directory) {
    return last_entry(value->text, is_same_directory, directory).found;
}

static int needs_separator(const wchar_t *text) {
    size_t length = wcslen(text);
    return length > 0 && text[length - 1] != L';';
}

static wchar_t *with_entry(const wchar_t *text, const wchar_t *entry, int separated) {
    size_t length = wcslen(text);
    size_t entry_length = wcslen(entry);
    wchar_t *joined = malloc((length + entry_length + 2) * sizeof(wchar_t));
    if (joined == NULL) {
        return NULL;
    }
    wmemcpy(joined, text, length);
    size_t used = length;
    if (separated) {
        joined[used++] = L';';
    }
    wmemcpy(joined + used, entry, entry_length);
    joined[used + entry_length] = L'\0';
    return joined;
}

/**
 * text with the entry at span cut out, along with the ; after it, or, when it is the last entry, the
 * ; the host added before it.
 */
static wchar_t *without_entry(const wchar_t *text, entry_span span, int separated) {
    size_t length = wcslen(text);
    size_t cut_start = span.start;
    size_t cut_end = span.end;
    if (span.end < length) {
        cut_end++;
    } else if (separated && span.start > 0) {
        cut_start--;
    }
    size_t kept = length - (cut_end - cut_start);
    wchar_t *remaining = malloc((kept + 1) * sizeof(wchar_t));
    if (remaining != NULL) {
        wmemcpy(remaining, text, cut_start);
        wmemcpy(remaining + cut_start, text + cut_end, length - cut_end);
        remaining[kept] = L'\0';
    }
    return remaining;
}

static int set_flag(cJSON *record, const char *member, int flag) {
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(record, member);
    if (existing != NULL) {
        cJSON_SetBoolValue(existing, flag);
        return 1;
    }
    return cJSON_AddBoolToObject(record, member, flag) != NULL;
}

static int is_flag_set(const cJSON *record, const char *member) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(record, member));
}

static int note_placement(cJSON *record, const path_value *value) {
    return set_flag(record, "separated", needs_separator(value->text)) && set_flag(record, "created", !value->present);
}

/**
 * @implNote A directory a holder already put there inherits the holder's entry, separator and
 * creation, so whichever record is reversed last takes away exactly what the first one added.
 */
static int inherit_placement(cJSON *record, const cJSON *holder) {
    const char *entry = text_of(holder, "entry");
    return cJSON_AddStringToObject(record, "entry", entry != NULL ? entry : text_of(record, "directory")) != NULL &&
           set_flag(record, "separated", is_flag_set(holder, "separated")) &&
           set_flag(record, "created", is_flag_set(holder, "created"));
}

static cJSON *new_record(const cJSON *change, const cJSON *holder, const path_value *value, int present) {
    cJSON *record = cJSON_CreateObject();
    int complete = record != NULL && cJSON_AddStringToObject(record, "directory", text_of(change, "directory")) &&
                   cJSON_AddStringToObject(record, "home", text_of(change, "home"));
    if (complete && present) {
        complete = inherit_placement(record, holder);
    } else if (complete) {
        complete = cJSON_AddStringToObject(record, "entry", text_of(change, "directory")) != NULL &&
                   note_placement(record, value);
    }
    if (!complete) {
        cJSON_Delete(record);
        return NULL;
    }
    return record;
}

static int read_environment(HKEY *key, path_value *value) {
    *key = open_environment();
    if (*key != NULL && read_path_value(*key, value)) {
        return 1;
    }
    if (*key != NULL) {
        RegCloseKey(*key);
    }
    return 0;
}

static void close_environment(HKEY key, path_value *value) {
    free(value->text);
    RegCloseKey(key);
}

runtime_change_step runtime_path_entry_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                               runtime_change_note *note) {
    const char *directory = text_of(change, "directory");
    HKEY key;
    path_value value;
    if (!read_environment(&key, &value)) {
        return fail(note, "the user's Path could not be read as a string value", directory);
    }
    int present = holds_directory(&value, directory);
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    if (present && holder == NULL) {
        step = RUNTIME_CHANGE_UNNEEDED;
    } else {
        *record = new_record(change, holder, &value, present);
        step = *record != NULL ? RUNTIME_CHANGE_DONE : fail(note, "memory ran out", directory);
    }
    close_environment(key, &value);
    return step;
}

static int append_entry(HKEY key, const path_value *value, cJSON *record) {
    wchar_t *entry = to_wide(text_of(record, "entry"));
    int separated = needs_separator(value->text);
    wchar_t *joined = entry == NULL ? NULL : with_entry(value->text, entry, separated);
    int stored = joined != NULL && note_placement(record, value) &&
                 store_path_text(key, value->present ? value->type : REG_EXPAND_SZ, joined);
    free(joined);
    free(entry);
    return stored;
}

runtime_change_step runtime_path_entry_perform(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *directory = text_of(change, "directory");
    HKEY key;
    path_value value;
    if (!read_environment(&key, &value)) {
        return fail(note, "the user's Path could not be read as a string value", directory);
    }
    int done = holds_directory(&value, directory) || append_entry(key, &value, record);
    close_environment(key, &value);
    return done ? RUNTIME_CHANGE_DONE : fail(note, "the directory could not be added to the user's Path", directory);
}

int runtime_path_entry_is_on_disk(const cJSON *record) {
    HKEY key;
    path_value value;
    if (!read_environment(&key, &value)) {
        return 0;
    }
    int on_disk = holds_directory(&value, text_of(record, "directory"));
    close_environment(key, &value);
    return on_disk;
}

static int remove_entry(HKEY key, const path_value *value, const cJSON *record) {
    wchar_t *entry = to_wide(text_of(record, "entry"));
    if (entry == NULL) {
        return 0;
    }
    entry_span span = last_entry(value->text, is_exact_entry, entry);
    free(entry);
    if (!span.found) {
        return 1;
    }
    wchar_t *remaining = without_entry(value->text, span, is_flag_set(record, "separated"));
    int removed = remaining != NULL && (remaining[0] == L'\0' && is_flag_set(record, "created")
                                            ? delete_path_value(key)
                                            : store_path_text(key, value->type, remaining));
    free(remaining);
    return removed;
}

runtime_change_reversal runtime_path_entry_reverse(const cJSON *record, int held, runtime_change_note *note) {
    if (held) {
        return RUNTIME_CHANGE_REVERSED;
    }
    const char *directory = text_of(record, "directory");
    HKEY key;
    path_value value;
    if (!read_environment(&key, &value)) {
        write_note(note, "the user's Path could not be read as a string value", directory);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    int removed = remove_entry(key, &value, record);
    close_environment(key, &value);
    if (!removed) {
        write_note(note, "the directory could not be taken off the user's Path", directory);
        return RUNTIME_CHANGE_NOT_REVERSED;
    }
    return RUNTIME_CHANGE_REVERSED;
}
