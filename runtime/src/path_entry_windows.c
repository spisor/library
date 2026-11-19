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
#include "allocation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "spisor/status.h"
#include "wide_path.h"

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

typedef int (*entry_matcher)(const wchar_t *entry, size_t length, void *wanted);

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

static runtime_change_step run_out_of_memory(runtime_change_note *note, const char *directory) {
    write_note(note, "memory ran out", directory);
    return RUNTIME_CHANGE_OUT_OF_MEMORY;
}

static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = length > 0 ? spisor_memory_allocate((size_t)length * sizeof(wchar_t)) : NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        spisor_memory_release(wide);
        wide = NULL;
    }
    return wide;
}

static char *to_narrow(const wchar_t *text, size_t length) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, NULL, 0, NULL, NULL);
    char *narrow = size > 0 ? spisor_memory_allocate((size_t)size + 1) : NULL;
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, text, (int)length, narrow, size, NULL, NULL) <= 0) {
        spisor_memory_release(narrow);
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

static spisor_status read_absent(path_value *value) {
    value->type = REG_EXPAND_SZ;
    value->present = 0;
    value->text = spisor_memory_allocate_zeroed(1, sizeof(wchar_t));
    return value->text == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

/**
 * Reads the value as stored, never expanded, so a REG_EXPAND_SZ written back keeps its %VARIABLES%.
 * An absent value reads as empty, typed REG_EXPAND_SZ as Windows creates it. A value of any other
 * type is a failure, so it is never overwritten.
 *
 * @implNote The size is asked first and the read retried, since the value can grow between the two calls.
 */
static spisor_status read_path_value(HKEY key, path_value *value) {
    value->text = NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        DWORD size = 0;
        LSTATUS status = RegGetValueW(key, NULL, path_value_name, READABLE_TYPES, &value->type, NULL, &size);
        if (status == ERROR_FILE_NOT_FOUND) {
            return read_absent(value);
        }
        if (status != ERROR_SUCCESS) {
            return SPISOR_REFUSED;
        }
        wchar_t *text = spisor_memory_allocate(size + sizeof(wchar_t));
        if (text == NULL) {
            return SPISOR_OUT_OF_MEMORY;
        }
        status = RegGetValueW(key, NULL, path_value_name, READABLE_TYPES, &value->type, text, &size);
        if (status == ERROR_SUCCESS) {
            value->text = text;
            value->present = 1;
            return SPISOR_OK;
        }
        spisor_memory_release(text);
        if (status != ERROR_MORE_DATA) {
            return SPISOR_REFUSED;
        }
    }
    return SPISOR_REFUSED;
}

#ifdef SPISOR_PATH_ENTRY_TEST_HOOKS
/** The test value is read by no program, so nothing is told it changed, which keeps a test from waiting on windows. */
static void announce_environment_change(void) {
}
#else
static void announce_environment_change(void) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG,
                        BROADCAST_TIMEOUT_MILLISECONDS, &result);
}
#endif

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

typedef struct directory_match {
    const char *directory;
    int out_of_memory;
} directory_match;

static int is_same_directory(const wchar_t *entry, size_t length, void *wanted) {
    directory_match *match = wanted;
    char *narrow = to_narrow(entry, length);
    if (narrow == NULL) {
        match->out_of_memory = 1;
        return 0;
    }
    int same = runtime_change_paths_equal(narrow, match->directory);
    spisor_memory_release(narrow);
    return same;
}

static int is_exact_entry(const wchar_t *entry, size_t length, void *wanted) {
    const wchar_t *exact = wanted;
    return wcslen(exact) == length && wmemcmp(entry, exact, length) == 0;
}

static entry_span last_entry(const wchar_t *text, entry_matcher matches, void *wanted) {
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

static runtime_change_presence holds_directory(const path_value *value, const char *directory) {
    directory_match match = {directory, 0};
    int found = last_entry(value->text, is_same_directory, &match).found;
    if (match.out_of_memory) {
        return RUNTIME_CHANGE_PRESENCE_OUT_OF_MEMORY;
    }
    return found ? RUNTIME_CHANGE_PRESENT : RUNTIME_CHANGE_ABSENT;
}

static int needs_separator(const wchar_t *text) {
    size_t length = wcslen(text);
    return length > 0 && text[length - 1] != L';';
}

static wchar_t *with_entry(const wchar_t *text, const wchar_t *entry, int separated) {
    size_t length = wcslen(text);
    size_t entry_length = wcslen(entry);
    wchar_t *joined = spisor_memory_allocate((length + entry_length + 2) * sizeof(wchar_t));
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
    wchar_t *remaining = spisor_memory_allocate((kept + 1) * sizeof(wchar_t));
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

static spisor_status read_environment(HKEY *key, path_value *value) {
    *key = open_environment();
    spisor_status read = *key == NULL ? SPISOR_REFUSED : read_path_value(*key, value);
    if (read != SPISOR_OK && *key != NULL) {
        RegCloseKey(*key);
    }
    return read;
}

static runtime_change_step unread_environment(spisor_status read, runtime_change_note *note, const char *directory) {
    return read == SPISOR_OUT_OF_MEMORY ? run_out_of_memory(note, directory)
                                        : fail(note, "the user's Path could not be read as a string value", directory);
}

static void close_environment(HKEY key, path_value *value) {
    spisor_memory_release(value->text);
    RegCloseKey(key);
}

runtime_change_step runtime_path_entry_prepare(const cJSON *change, const cJSON *holder, cJSON **record,
                                               runtime_change_note *note) {
    const char *directory = text_of(change, "directory");
    HKEY key;
    path_value value;
    spisor_status read = read_environment(&key, &value);
    if (read != SPISOR_OK) {
        return unread_environment(read, note, directory);
    }
    runtime_change_presence presence = holds_directory(&value, directory);
    int present = presence == RUNTIME_CHANGE_PRESENT;
    runtime_change_step step = RUNTIME_CHANGE_DONE;
    if (presence == RUNTIME_CHANGE_PRESENCE_OUT_OF_MEMORY) {
        step = run_out_of_memory(note, directory);
    } else if (present && holder == NULL) {
        step = RUNTIME_CHANGE_UNNEEDED;
    } else {
        *record = new_record(change, holder, &value, present);
        step = *record != NULL ? RUNTIME_CHANGE_DONE : run_out_of_memory(note, directory);
    }
    close_environment(key, &value);
    return step;
}

/** Appends the record's entry to the value: SPISOR_REFUSED when it cannot be stored, or SPISOR_OUT_OF_MEMORY. */
static spisor_status append_entry(HKEY key, const path_value *value, cJSON *record) {
    const char *text = text_of(record, "entry");
    wchar_t *entry = to_wide(text);
    int separated = needs_separator(value->text);
    wchar_t *joined = entry == NULL ? NULL : with_entry(value->text, entry, separated);
    spisor_status stored = SPISOR_OUT_OF_MEMORY;
    if (entry == NULL && !spisor_is_convertible_path(text)) {
        stored = SPISOR_REFUSED;
    } else if (joined != NULL && note_placement(record, value)) {
        stored = store_path_text(key, value->present ? value->type : REG_EXPAND_SZ, joined) ? SPISOR_OK
                                                                                            : SPISOR_REFUSED;
    }
    spisor_memory_release(joined);
    spisor_memory_release(entry);
    return stored;
}

runtime_change_step runtime_path_entry_perform(const cJSON *change, cJSON *record, runtime_change_note *note) {
    const char *directory = text_of(change, "directory");
    HKEY key;
    path_value value;
    spisor_status read = read_environment(&key, &value);
    if (read != SPISOR_OK) {
        return unread_environment(read, note, directory);
    }
    runtime_change_presence presence = holds_directory(&value, directory);
    spisor_status done = SPISOR_OUT_OF_MEMORY;
    if (presence != RUNTIME_CHANGE_PRESENCE_OUT_OF_MEMORY) {
        done = presence == RUNTIME_CHANGE_PRESENT ? SPISOR_OK : append_entry(key, &value, record);
    }
    close_environment(key, &value);
    if (done == SPISOR_OUT_OF_MEMORY) {
        return run_out_of_memory(note, directory);
    }
    return done == SPISOR_OK ? RUNTIME_CHANGE_DONE
                             : fail(note, "the directory could not be added to the user's Path", directory);
}

runtime_change_presence runtime_path_entry_is_on_disk(const cJSON *record) {
    HKEY key;
    path_value value;
    spisor_status read = read_environment(&key, &value);
    if (read != SPISOR_OK) {
        return read == SPISOR_OUT_OF_MEMORY ? RUNTIME_CHANGE_PRESENCE_OUT_OF_MEMORY : RUNTIME_CHANGE_ABSENT;
    }
    runtime_change_presence on_disk = holds_directory(&value, text_of(record, "directory"));
    close_environment(key, &value);
    return on_disk;
}

static int remove_entry(HKEY key, const path_value *value, const cJSON *record) {
    wchar_t *entry = to_wide(text_of(record, "entry"));
    if (entry == NULL) {
        return 0;
    }
    entry_span span = last_entry(value->text, is_exact_entry, entry);
    spisor_memory_release(entry);
    if (!span.found) {
        return 1;
    }
    wchar_t *remaining = without_entry(value->text, span, is_flag_set(record, "separated"));
    int removed = remaining != NULL && (remaining[0] == L'\0' && is_flag_set(record, "created")
                                            ? delete_path_value(key)
                                            : store_path_text(key, value->type, remaining));
    spisor_memory_release(remaining);
    return removed;
}

spisor_status runtime_path_entry_prepare_undo(runtime_path_entry_undo *undo) {
    HKEY key;
    path_value value;
    undo->text = NULL;
    undo->type = REG_EXPAND_SZ;
    undo->present = 0;
    spisor_status read = read_environment(&key, &value);
    if (read != SPISOR_OK) {
        return read;
    }
    undo->text = value.text;
    undo->type = value.type;
    undo->present = value.present;
    RegCloseKey(key);
    return SPISOR_OK;
}

int runtime_path_entry_restore(const runtime_path_entry_undo *undo) {
    HKEY key = open_environment();
    if (key == NULL) {
        return 0;
    }
    int restored = undo->present ? store_path_text(key, undo->type, undo->text) : delete_path_value(key);
    RegCloseKey(key);
    return restored;
}

void runtime_path_entry_release_undo(runtime_path_entry_undo *undo) {
    spisor_memory_release(undo->text);
    undo->text = NULL;
}

runtime_change_reversal runtime_path_entry_reverse(const cJSON *record, int held, runtime_change_note *note) {
    if (held) {
        return RUNTIME_CHANGE_REVERSED;
    }
    const char *directory = text_of(record, "directory");
    HKEY key;
    path_value value;
    spisor_status read = read_environment(&key, &value);
    if (read == SPISOR_OUT_OF_MEMORY) {
        write_note(note, "memory ran out", directory);
        return RUNTIME_CHANGE_REVERSAL_OUT_OF_MEMORY;
    }
    if (read != SPISOR_OK) {
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
