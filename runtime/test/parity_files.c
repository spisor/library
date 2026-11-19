#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "parity_files.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "spisor/allocation.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct folder_entry {
    char *name;
    int is_folder;
} folder_entry;

typedef struct folder_listing {
    folder_entry *entries;
    size_t count;
    size_t capacity;
} folder_listing;

char *parity_join(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    char *joined = malloc(left_length + right_length + 2);
    if (joined != NULL) {
        memcpy(joined, left, left_length);
        joined[left_length] = '/';
        memcpy(joined + left_length + 1, right, right_length + 1);
    }
    return joined;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static int add_entry(folder_listing *listing, const char *name, int is_folder) {
    if (listing->count == listing->capacity) {
        size_t capacity = listing->capacity == 0 ? 8 : listing->capacity * 2;
        folder_entry *grown = realloc(listing->entries, capacity * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        listing->entries = grown;
        listing->capacity = capacity;
    }
    char *copy = copy_text(name);
    if (copy == NULL) {
        return 0;
    }
    listing->entries[listing->count].name = copy;
    listing->entries[listing->count].is_folder = is_folder;
    listing->count++;
    return 1;
}

static void free_listing(folder_listing *listing) {
    for (size_t index = 0; index < listing->count; index++) {
        free(listing->entries[index].name);
    }
    free(listing->entries);
    listing->entries = NULL;
    listing->count = 0;
    listing->capacity = 0;
}

static int compare_entries(const void *left, const void *right) {
    return strcmp(((const folder_entry *)left)->name, ((const folder_entry *)right)->name);
}

static int is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

#if defined(_WIN32)
static wchar_t *wide_of(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide = count > 0 ? malloc((size_t)count * sizeof *wide) : NULL;
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count) == 0) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

static char *utf8_of(const wchar_t *wide) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = count > 0 ? malloc((size_t)count) : NULL;
    if (text != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, count, NULL, NULL) == 0) {
        free(text);
        text = NULL;
    }
    return text;
}

static int is_real_folder(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/** 1 when `path` was listed, and also when nothing stands there, which lists as empty. */
static int list_folder(const char *path, folder_listing *listing) {
    char *pattern = parity_join(path, "*");
    wchar_t *wide_pattern = pattern == NULL ? NULL : wide_of(pattern);
    free(pattern);
    if (wide_pattern == NULL) {
        return 0;
    }
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wide_pattern, &found);
    free(wide_pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    int listed = 1;
    do {
        char *name = utf8_of(found.cFileName);
        int is_folder = is_real_folder(found.dwFileAttributes);
        listed = name != NULL && (is_dot_entry(name) || add_entry(listing, name, is_folder));
        free(name);
    } while (listed && FindNextFileW(handle, &found));
    FindClose(handle);
    return listed;
}

static int remove_file_entry(const char *path, int is_folder) {
    wchar_t *wide = wide_of(path);
    if (wide == NULL) {
        return 0;
    }
    SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
    BOOL removed = is_folder ? RemoveDirectoryW(wide) : DeleteFileW(wide);
    free(wide);
    return removed != 0;
}

/** -1 when nothing stands at `path`, 1 for a real folder, 0 for anything else. */
static int folder_state(const char *path) {
    wchar_t *wide = wide_of(path);
    DWORD attributes = wide == NULL ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(wide);
    free(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return -1;
    }
    return is_real_folder(attributes);
}
#else
static int is_real_folder_at(const char *path) {
    struct stat status;
    return lstat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

/** 1 when `path` was listed, and also when nothing stands there, which lists as empty. */
static int list_folder(const char *path, folder_listing *listing) {
    DIR *handle = opendir(path);
    if (handle == NULL) {
        return errno == ENOENT;
    }
    int listed = 1;
    struct dirent *entry = NULL;
    while (listed && (entry = readdir(handle)) != NULL) {
        if (is_dot_entry(entry->d_name)) {
            continue;
        }
        char *entry_path = parity_join(path, entry->d_name);
        listed = entry_path != NULL && add_entry(listing, entry->d_name, is_real_folder_at(entry_path));
        free(entry_path);
    }
    closedir(handle);
    return listed;
}

static int remove_file_entry(const char *path, int is_folder) {
    return (is_folder ? rmdir(path) : unlink(path)) == 0;
}

/** -1 when nothing stands at `path`, 1 for a real folder, 0 for anything else. */
static int folder_state(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0) {
        return -1;
    }
    return S_ISDIR(status.st_mode) ? 1 : 0;
}
#endif

static int sorted_listing(const char *path, folder_listing *listing) {
    if (!list_folder(path, listing)) {
        free_listing(listing);
        return 0;
    }
    if (listing->count > 1) {
        qsort(listing->entries, listing->count, sizeof *listing->entries, compare_entries);
    }
    return 1;
}

static int remove_folder_contents(const char *path) {
    folder_listing listing = {NULL, 0, 0};
    if (!sorted_listing(path, &listing)) {
        return 0;
    }
    int removed = 1;
    for (size_t index = 0; removed && index < listing.count; index++) {
        char *entry_path = parity_join(path, listing.entries[index].name);
        int is_folder = listing.entries[index].is_folder;
        removed = entry_path != NULL && (!is_folder || remove_folder_contents(entry_path)) &&
                  remove_file_entry(entry_path, is_folder);
        free(entry_path);
    }
    free_listing(&listing);
    return removed;
}

int parity_remove_tree(const char *path) {
    int state = folder_state(path);
    if (state < 0) {
        return 1;
    }
    return (state == 0 || remove_folder_contents(path)) && remove_file_entry(path, state);
}

static int make_parent_folders(const char *path) {
    char *parent = copy_text(path);
    char *last_slash = parent == NULL ? NULL : strrchr(parent, '/');
    int made = last_slash != NULL;
    if (made) {
        *last_slash = '\0';
        made = spisor_make_directories(parent) == SPISOR_OK;
    }
    free(parent);
    return made;
}

int parity_write_file(const char *path, const char *bytes, size_t length) {
    return make_parent_folders(path) && spisor_replace_file(path, (const uint8_t *)bytes, length) == SPISOR_OK;
}

int parity_copy_file(const char *source, const char *target) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(source, &length, &outcome);
    int copied = bytes != NULL && parity_write_file(target, (const char *)bytes, length);
    spisor_memory_release(bytes);
    return copied;
}

cJSON *parity_names_in(const char *path, int folders) {
    folder_listing listing = {NULL, 0, 0};
    cJSON *names = cJSON_CreateArray();
    int listed = names != NULL && sorted_listing(path, &listing);
    for (size_t index = 0; listed && index < listing.count; index++) {
        if (listing.entries[index].is_folder == folders) {
            listed = cJSON_AddItemToArray(names, cJSON_CreateString(listing.entries[index].name));
        }
    }
    free_listing(&listing);
    if (!listed) {
        cJSON_Delete(names);
        return NULL;
    }
    return names;
}

static int add_file(const char *path, const char *relative, cJSON *files) {
    size_t length = 0;
    spisor_read_outcome outcome = SPISOR_READ_FAILED;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    int added = bytes != NULL && cJSON_AddItemToObject(files, relative, cJSON_CreateString((const char *)bytes));
    spisor_memory_release(bytes);
    return added;
}

int parity_add_files_under(const char *home, const char *relative, cJSON *files) {
    char *path = parity_join(home, relative);
    folder_listing listing = {NULL, 0, 0};
    int added = path != NULL && (folder_state(path) != 1 || sorted_listing(path, &listing));
    for (size_t index = 0; added && index < listing.count; index++) {
        char *entry_relative = parity_join(relative, listing.entries[index].name);
        char *entry_path = entry_relative == NULL ? NULL : parity_join(home, entry_relative);
        if (entry_path == NULL) {
            added = 0;
        } else if (listing.entries[index].is_folder) {
            added = parity_add_files_under(home, entry_relative, files);
        } else {
            added = add_file(entry_path, entry_relative, files);
        }
        free(entry_path);
        free(entry_relative);
    }
    free_listing(&listing);
    free(path);
    return added;
}
