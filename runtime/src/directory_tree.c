#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "directory_tree.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "memory_error.h"
#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>

#include "wide_path.h"
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct entry_buffer {
    runtime_directory_entry *entries;
    size_t count;
    size_t capacity;
} entry_buffer;

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = spisor_memory_allocate(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static int is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int grow(entry_buffer *buffer) {
    if (buffer->count < buffer->capacity) {
        return 1;
    }
    size_t capacity = buffer->capacity == 0 ? 16 : buffer->capacity * 2;
    runtime_directory_entry *grown = spisor_memory_reallocate(buffer->entries, capacity * sizeof(*grown));
    if (grown == NULL) {
        return 0;
    }
    buffer->entries = grown;
    buffer->capacity = capacity;
    return 1;
}

static int append(entry_buffer *buffer, const char *name, int is_directory) {
    if (!grow(buffer)) {
        return 0;
    }
    char *owned = copy_text(name);
    if (owned == NULL) {
        return 0;
    }
    buffer->entries[buffer->count].name = owned;
    buffer->entries[buffer->count].is_directory = is_directory;
    buffer->count++;
    return 1;
}

static int compare_entries(const void *left, const void *right) {
    return strcmp(((const runtime_directory_entry *)left)->name, ((const runtime_directory_entry *)right)->name);
}

static char *child_path(const char *parent, const char *name) {
    const char *segments[2] = {parent, name};
    return spisor_path_join(RUNTIME_HOST_IS_WINDOWS, segments, 2);
}

void runtime_free_directory_listing(runtime_directory_listing *listing) {
    for (size_t index = 0; index < listing->count; index++) {
        spisor_memory_release(listing->entries[index].name);
    }
    spisor_memory_release(listing->entries);
    listing->entries = NULL;
    listing->count = 0;
}

static spisor_status finish_listing(entry_buffer *buffer, spisor_status status, runtime_directory_listing *listing) {
    if (status != SPISOR_OK) {
        runtime_directory_listing partial = {buffer->entries, buffer->count};
        runtime_free_directory_listing(&partial);
        return status;
    }
    if (buffer->count > 1) {
        qsort(buffer->entries, buffer->count, sizeof(*buffer->entries), compare_entries);
    }
    listing->entries = buffer->entries;
    listing->count = buffer->count;
    return SPISOR_OK;
}

static spisor_status remove_children(const char *path);

#if defined(_WIN32)

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = spisor_memory_allocate((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        spisor_memory_release(narrow);
        return NULL;
    }
    return narrow;
}

static int is_reparse_point(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static int is_real_directory(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !is_reparse_point(attributes);
}

static spisor_status read_found_entries(HANDLE handle, WIN32_FIND_DATAW *found, entry_buffer *buffer) {
    spisor_status status = SPISOR_OK;
    do {
        char *name = to_utf8(found->cFileName);
        int is_directory = is_real_directory(found->dwFileAttributes);
        if (name == NULL || (!is_dot_entry(name) && !append(buffer, name, is_directory))) {
            status = SPISOR_OUT_OF_MEMORY;
        }
        spisor_memory_release(name);
    } while (status == SPISOR_OK && FindNextFileW(handle, found));
    return status == SPISOR_OK && GetLastError() != ERROR_NO_MORE_FILES ? SPISOR_REFUSED : status;
}

static spisor_status wide_pattern_of(const char *path, wchar_t **wide_pattern) {
    *wide_pattern = NULL;
    if (!spisor_is_convertible_path(path)) {
        return SPISOR_REFUSED;
    }
    char *pattern = child_path(path, "*");
    *wide_pattern = pattern == NULL ? NULL : spisor_wide_path(pattern);
    spisor_memory_release(pattern);
    return *wide_pattern == NULL ? SPISOR_OUT_OF_MEMORY : SPISOR_OK;
}

spisor_status runtime_list_directory(const char *path, runtime_directory_listing *listing) {
    memset(listing, 0, sizeof(*listing));
    wchar_t *wide_pattern;
    spisor_status status = wide_pattern_of(path, &wide_pattern);
    if (status != SPISOR_OK) {
        return status;
    }
    entry_buffer buffer = {NULL, 0, 0};
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wide_pattern, &found);
    DWORD reason = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    spisor_memory_release(wide_pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return spisor_is_memory_windows_error(reason)                                ? SPISOR_OUT_OF_MEMORY
               : reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND ? SPISOR_OK
                                                                                  : SPISOR_REFUSED;
    }
    status = read_found_entries(handle, &found, &buffer);
    FindClose(handle);
    return finish_listing(&buffer, status, listing);
}

/** The UTF-16 form of path, freed; NULL with *out_of_memory set to 1 when memory, not the path, was the cause. */
static wchar_t *wide_of(const char *path, int *out_of_memory) {
    *out_of_memory = 0;
    if (!spisor_is_convertible_path(path)) {
        return NULL;
    }
    wchar_t *wide = spisor_wide_path(path);
    *out_of_memory = wide == NULL;
    return wide;
}

static int delete_link_at(const wchar_t *wide, int is_directory) {
    return (is_directory ? RemoveDirectoryW(wide) : DeleteFileW(wide)) != 0;
}

/**
 * @implNote Attributes are reset only after an access-denied failure, and never on a link, because
 * SetFileAttributesW follows a link and would change the attributes of its target.
 */
static int delete_file_at(const wchar_t *wide) {
    int deleted = DeleteFileW(wide) != 0;
    if (!deleted && GetLastError() == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
        deleted = DeleteFileW(wide) != 0;
    }
    return deleted;
}

static spisor_status removed_if(int removed) {
    return removed ? SPISOR_OK : SPISOR_REFUSED;
}

static spisor_status remove_tree_at(const char *path, const wchar_t *wide) {
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        spisor_file_presence presence = spisor_probe_file(path);
        return presence == SPISOR_FILE_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY
                                                     : removed_if(presence != SPISOR_FILE_PRESENT);
    }
    if (is_reparse_point(attributes)) {
        return removed_if(delete_link_at(wide, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return removed_if(delete_file_at(wide));
    }
    spisor_status emptied = remove_children(path);
    return emptied == SPISOR_OK ? removed_if(RemoveDirectoryW(wide) != 0) : emptied;
}

spisor_status runtime_remove_tree(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
    }
    spisor_status removed = remove_tree_at(path, wide);
    spisor_memory_release(wide);
    return removed;
}

static int is_missing_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static runtime_entry_kind kind_of(DWORD attributes, DWORD error) {
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return is_missing_error(error) ? RUNTIME_ENTRY_MISSING : RUNTIME_ENTRY_UNKNOWN;
    }
    return is_real_directory(attributes) ? RUNTIME_ENTRY_DIRECTORY : RUNTIME_ENTRY_OTHER;
}

runtime_entry_kind runtime_probe_entry(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return out_of_memory ? RUNTIME_ENTRY_PROBE_OUT_OF_MEMORY : RUNTIME_ENTRY_UNKNOWN;
    }
    DWORD attributes = GetFileAttributesW(wide);
    DWORD error = GetLastError();
    spisor_memory_release(wide);
    return kind_of(attributes, error);
}

static runtime_entry_outcome unconverted(int out_of_memory) {
    return out_of_memory ? RUNTIME_ENTRY_OUT_OF_MEMORY : RUNTIME_ENTRY_FAILED;
}

static int write_all(HANDLE handle, const uint8_t *bytes, size_t length) {
    size_t written = 0;
    while (written < length) {
        DWORD chunk = length - written > 0x40000000 ? 0x40000000 : (DWORD)(length - written);
        DWORD wrote = 0;
        if (!WriteFile(handle, bytes + written, chunk, &wrote, NULL) || wrote == 0) {
            return 0;
        }
        written += wrote;
    }
    return 1;
}

#define DELETE_PENDING_STATUS ((LONG)0xC0000056L)

typedef LONG(WINAPI *nt_status_reader)(void);

static INIT_ONCE nt_status_reader_once = INIT_ONCE_STATIC_INIT;
static nt_status_reader resolved_nt_status_reader = NULL;

static BOOL CALLBACK resolve_nt_status_reader(PINIT_ONCE once, PVOID unused, PVOID *unused_context) {
    (void)once;
    (void)unused;
    (void)unused_context;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    resolved_nt_status_reader =
        ntdll == NULL ? NULL : (nt_status_reader)(void *)GetProcAddress(ntdll, "RtlGetLastNtStatus");
    return TRUE;
}

/** @implNote Resolved before the call whose status it reads, since resolving afterwards could overwrite it. */
static nt_status_reader nt_status_reader_of_process(void) {
    InitOnceExecuteOnce(&nt_status_reader_once, resolve_nt_status_reader, NULL, NULL);
    return resolved_nt_status_reader;
}

/**
 * Whether a create that failed met a file deleted while something still holds it open: its name stands until
 * the last handle closes, and Win32 reports that only as access denied.
 */
static int met_a_file_being_deleted(DWORD error, LONG status) {
    return error == ERROR_ACCESS_DENIED && status == DELETE_PENDING_STATUS;
}

runtime_entry_outcome runtime_create_new_file(const char *path, const uint8_t *bytes, size_t length, int executable) {
    (void)executable;
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return unconverted(out_of_memory);
    }
    nt_status_reader reader = nt_status_reader_of_process();
    HANDLE handle = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LONG status = reader == NULL ? 0 : reader();
        spisor_memory_release(wide);
        int exists = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ||
                     met_a_file_being_deleted(error, status);
        return exists ? RUNTIME_ENTRY_EXISTS : RUNTIME_ENTRY_FAILED;
    }
    int written = write_all(handle, bytes, length);
    int closed = CloseHandle(handle) != 0;
    if (!written || !closed) {
        DeleteFileW(wide);
    }
    spisor_memory_release(wide);
    return written && closed ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_create_directory(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return unconverted(out_of_memory);
    }
    int created = CreateDirectoryW(wide, NULL) != 0;
    DWORD attributes = GetFileAttributesW(wide);
    spisor_memory_release(wide);
    int is_directory = attributes != INVALID_FILE_ATTRIBUTES && is_real_directory(attributes);
    return created || is_directory ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

static runtime_entry_outcome remove_file_entry_at(const wchar_t *wide) {
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return is_missing_error(GetLastError()) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
    }
    if (is_reparse_point(attributes)) {
        return delete_link_at(wide, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ? RUNTIME_ENTRY_DONE
                                                                                   : RUNTIME_ENTRY_FAILED;
    }
    if (is_real_directory(attributes)) {
        return RUNTIME_ENTRY_KEPT;
    }
    return delete_file_at(wide) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_remove_file_entry(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return unconverted(out_of_memory);
    }
    runtime_entry_outcome outcome = remove_file_entry_at(wide);
    spisor_memory_release(wide);
    return outcome;
}

static runtime_entry_outcome remove_empty_directory_at(const wchar_t *wide) {
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return is_missing_error(GetLastError()) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
    }
    if (!is_real_directory(attributes)) {
        return RUNTIME_ENTRY_KEPT;
    }
    if (RemoveDirectoryW(wide)) {
        return RUNTIME_ENTRY_DONE;
    }
    return GetLastError() == ERROR_DIR_NOT_EMPTY ? RUNTIME_ENTRY_KEPT : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_remove_empty_directory(const char *path) {
    int out_of_memory;
    wchar_t *wide = wide_of(path, &out_of_memory);
    if (wide == NULL) {
        return unconverted(out_of_memory);
    }
    runtime_entry_outcome outcome = remove_empty_directory_at(wide);
    spisor_memory_release(wide);
    return outcome;
}

#else

static int append_directory_entry(entry_buffer *buffer, const char *parent, const char *name) {
    char *full = child_path(parent, name);
    struct stat status;
    int is_directory = full != NULL && lstat(full, &status) == 0 && S_ISDIR(status.st_mode);
    spisor_memory_release(full);
    return full != NULL && append(buffer, name, is_directory);
}

spisor_status runtime_list_directory(const char *path, runtime_directory_listing *listing) {
    memset(listing, 0, sizeof(*listing));
    DIR *handle = opendir(path);
    if (handle == NULL) {
        return spisor_is_memory_errno(errno)             ? SPISOR_OUT_OF_MEMORY
               : errno == ENOENT || errno == ENOTDIR ? SPISOR_OK
                                                     : SPISOR_REFUSED;
    }
    entry_buffer buffer = {NULL, 0, 0};
    int complete = 1;
    struct dirent *entry;
    while (complete && (entry = readdir(handle)) != NULL) {
        complete = is_dot_entry(entry->d_name) || append_directory_entry(&buffer, path, entry->d_name);
    }
    closedir(handle);
    return finish_listing(&buffer, complete ? SPISOR_OK : SPISOR_OUT_OF_MEMORY, listing);
}

spisor_status runtime_remove_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return errno == ENOENT || errno == ENOTDIR ? SPISOR_OK : SPISOR_REFUSED;
    }
    if (!S_ISDIR(status.st_mode)) {
        return unlink(path) == 0 ? SPISOR_OK : SPISOR_REFUSED;
    }
    spisor_status emptied = remove_children(path);
    if (emptied != SPISOR_OK) {
        return emptied;
    }
    return rmdir(path) == 0 ? SPISOR_OK : SPISOR_REFUSED;
}

static int is_missing_errno(int error) {
    return error == ENOENT || error == ENOTDIR;
}

runtime_entry_kind runtime_probe_entry(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return is_missing_errno(errno) ? RUNTIME_ENTRY_MISSING : RUNTIME_ENTRY_UNKNOWN;
    }
    return S_ISDIR(status.st_mode) ? RUNTIME_ENTRY_DIRECTORY : RUNTIME_ENTRY_OTHER;
}

static int write_all(int descriptor, const uint8_t *bytes, size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t wrote = write(descriptor, bytes + written, length - written);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return 0;
        }
        written += (size_t)wrote;
    }
    return 1;
}

runtime_entry_outcome runtime_create_new_file(const char *path, const uint8_t *bytes, size_t length, int executable) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, executable ? 0777 : 0666);
    if (descriptor < 0) {
        return errno == EEXIST ? RUNTIME_ENTRY_EXISTS : RUNTIME_ENTRY_FAILED;
    }
    int written = write_all(descriptor, bytes, length);
    int closed = close(descriptor) == 0;
    if (!written || !closed) {
        unlink(path);
    }
    return written && closed ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_create_directory(const char *path) {
    int created = mkdir(path, 0777) == 0;
    return created || runtime_probe_entry(path) == RUNTIME_ENTRY_DIRECTORY ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_remove_file_entry(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return is_missing_errno(errno) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
    }
    if (S_ISDIR(status.st_mode)) {
        return RUNTIME_ENTRY_KEPT;
    }
    return unlink(path) == 0 ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
}

runtime_entry_outcome runtime_remove_empty_directory(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return is_missing_errno(errno) ? RUNTIME_ENTRY_DONE : RUNTIME_ENTRY_FAILED;
    }
    if (!S_ISDIR(status.st_mode)) {
        return RUNTIME_ENTRY_KEPT;
    }
    if (rmdir(path) == 0) {
        return RUNTIME_ENTRY_DONE;
    }
    return errno == ENOTEMPTY || errno == EEXIST ? RUNTIME_ENTRY_KEPT : RUNTIME_ENTRY_FAILED;
}

#endif

static spisor_status remove_children(const char *path) {
    runtime_directory_listing listing;
    spisor_status removed = runtime_list_directory(path, &listing);
    for (size_t index = 0; removed == SPISOR_OK && index < listing.count; index++) {
        char *child = child_path(path, listing.entries[index].name);
        removed = child == NULL ? SPISOR_OUT_OF_MEMORY : runtime_remove_tree(child);
        spisor_memory_release(child);
    }
    runtime_free_directory_listing(&listing);
    return removed;
}
