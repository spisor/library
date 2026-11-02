#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "directory_tree.h"

#include <stdlib.h>
#include <string.h>

#include "paths.h"
#include "spisor/file.h"
#include "spisor/path.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
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
    char *copy = malloc(size);
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
    runtime_directory_entry *grown = realloc(buffer->entries, capacity * sizeof(*grown));
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
        free(listing->entries[index].name);
    }
    free(listing->entries);
    listing->entries = NULL;
    listing->count = 0;
}

static int finish_listing(entry_buffer *buffer, int complete, runtime_directory_listing *listing) {
    if (!complete) {
        runtime_directory_listing partial = {buffer->entries, buffer->count};
        runtime_free_directory_listing(&partial);
        return 0;
    }
    if (buffer->count > 1) {
        qsort(buffer->entries, buffer->count, sizeof(*buffer->entries), compare_entries);
    }
    listing->entries = buffer->entries;
    listing->count = buffer->count;
    return 1;
}

static int remove_children(const char *path);

#if defined(_WIN32)

static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0) {
        return NULL;
    }
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *to_utf8(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) {
        return NULL;
    }
    char *narrow = malloc((size_t)length);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, length, NULL, NULL) <= 0) {
        free(narrow);
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

static int read_found_entries(HANDLE handle, WIN32_FIND_DATAW *found, entry_buffer *buffer) {
    int complete = 1;
    do {
        char *name = to_utf8(found->cFileName);
        if (name == NULL) {
            complete = 0;
        } else if (!is_dot_entry(name)) {
            complete = append(buffer, name, is_real_directory(found->dwFileAttributes));
        }
        free(name);
    } while (complete && FindNextFileW(handle, found));
    return complete && GetLastError() == ERROR_NO_MORE_FILES;
}

int runtime_list_directory(const char *path, runtime_directory_listing *listing) {
    memset(listing, 0, sizeof(*listing));
    char *pattern = child_path(path, "*");
    wchar_t *wide_pattern = pattern == NULL ? NULL : to_wide(pattern);
    free(pattern);
    if (wide_pattern == NULL) {
        return 0;
    }
    entry_buffer buffer = {NULL, 0, 0};
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wide_pattern, &found);
    free(wide_pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD reason = GetLastError();
        return reason == ERROR_FILE_NOT_FOUND || reason == ERROR_PATH_NOT_FOUND;
    }
    int complete = read_found_entries(handle, &found, &buffer);
    FindClose(handle);
    return finish_listing(&buffer, complete, listing);
}

static DWORD attributes_of(const char *path) {
    wchar_t *wide = to_wide(path);
    DWORD attributes = wide == NULL ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(wide);
    free(wide);
    return attributes;
}

static int delete_link_at(const char *path, int is_directory) {
    wchar_t *wide = to_wide(path);
    int deleted = wide != NULL && (is_directory ? RemoveDirectoryW(wide) : DeleteFileW(wide)) != 0;
    free(wide);
    return deleted;
}

/**
 * @implNote Attributes are reset only after an access-denied failure, and never on a link, because
 * SetFileAttributesW follows a link and would change the attributes of its target.
 */
static int delete_file_at(const char *path) {
    wchar_t *wide = to_wide(path);
    int deleted = wide != NULL && DeleteFileW(wide) != 0;
    if (!deleted && wide != NULL && GetLastError() == ERROR_ACCESS_DENIED) {
        SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
        deleted = DeleteFileW(wide) != 0;
    }
    free(wide);
    return deleted;
}

static int delete_directory_at(const char *path) {
    wchar_t *wide = to_wide(path);
    int deleted = wide != NULL && RemoveDirectoryW(wide) != 0;
    free(wide);
    return deleted;
}

int runtime_remove_tree(const char *path) {
    DWORD attributes = attributes_of(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return !spisor_file_exists(path);
    }
    if (is_reparse_point(attributes)) {
        return delete_link_at(path, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return delete_file_at(path);
    }
    return remove_children(path) && delete_directory_at(path);
}

#else

static int append_directory_entry(entry_buffer *buffer, const char *parent, const char *name) {
    char *full = child_path(parent, name);
    struct stat status;
    int is_directory = full != NULL && lstat(full, &status) == 0 && S_ISDIR(status.st_mode);
    free(full);
    return full != NULL && append(buffer, name, is_directory);
}

int runtime_list_directory(const char *path, runtime_directory_listing *listing) {
    memset(listing, 0, sizeof(*listing));
    DIR *handle = opendir(path);
    if (handle == NULL) {
        return errno == ENOENT || errno == ENOTDIR;
    }
    entry_buffer buffer = {NULL, 0, 0};
    int complete = 1;
    struct dirent *entry;
    while (complete && (entry = readdir(handle)) != NULL) {
        complete = is_dot_entry(entry->d_name) || append_directory_entry(&buffer, path, entry->d_name);
    }
    closedir(handle);
    return finish_listing(&buffer, complete, listing);
}

int runtime_remove_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return errno == ENOENT || errno == ENOTDIR;
    }
    if (!S_ISDIR(status.st_mode)) {
        return unlink(path) == 0;
    }
    return remove_children(path) && rmdir(path) == 0;
}

#endif

static int remove_children(const char *path) {
    runtime_directory_listing listing;
    int removed = runtime_list_directory(path, &listing);
    for (size_t index = 0; removed && index < listing.count; index++) {
        char *child = child_path(path, listing.entries[index].name);
        removed = child != NULL && runtime_remove_tree(child);
        free(child);
    }
    runtime_free_directory_listing(&listing);
    return removed;
}
