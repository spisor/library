#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

#define NON_ASCII_NAME SPISOR_SCRATCH_PATH "/caf\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac.txt"
#define CONTENT "round trip"

static int write_fixture(const char *utf8_path, const char *content) {
#if defined(_WIN32)
    wchar_t wide_path[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1, wide_path, 1024) == 0) {
        return 0;
    }
    FILE *file = _wfopen(wide_path, L"wb");
#else
    FILE *file = fopen(utf8_path, "wb");
#endif
    if (file == NULL) {
        return 0;
    }
    size_t written = fwrite(content, 1, strlen(content), file);
    return fclose(file) == 0 && written == strlen(content);
}

static void a_directory_exists(void) {
    CHECK(spisor_file_exists(SPISOR_SCRATCH_PATH) != 0);
    CHECK(spisor_file_exists(SPISOR_SCRATCH_PATH "/") != 0);
}

static void a_missing_file_does_not(void) {
    CHECK(spisor_file_exists(SPISOR_SCRATCH_PATH "/missing.json") == 0);
    CHECK(spisor_file_exists("") == 0);
    size_t length = 7;
    CHECK(spisor_read_file(SPISOR_SCRATCH_PATH "/missing.json", &length) == NULL);
}

static void a_directory_is_not_readable(void) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(SPISOR_SCRATCH_PATH, &length);
    CHECK(bytes == NULL);
    free(bytes);
}

static void a_non_ascii_name_round_trips(void) {
    CHECK(write_fixture(NON_ASCII_NAME, CONTENT));
    CHECK(spisor_file_exists(NON_ASCII_NAME) != 0);
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(NON_ASCII_NAME, &length);
    CHECK(bytes != NULL);
    CHECK(length == strlen(CONTENT));
    if (bytes != NULL) {
        check_text((const char *)bytes, CONTENT, "non-ASCII file content");
    }
    free(bytes);
}

static void an_empty_file_reads_as_empty(void) {
    const char *path = SPISOR_SCRATCH_PATH "/empty.txt";
    CHECK(write_fixture(path, ""));
    size_t length = 9;
    uint8_t *bytes = spisor_read_file(path, &length);
    CHECK(bytes != NULL);
    CHECK(length == 0);
    free(bytes);
}

static int count_temporary_entries(const char *directory) {
    int count = 0;
#if defined(_WIN32)
    char pattern[1024];
    wchar_t wide_pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.tmp", directory);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wide_pattern, 1024) == 0) {
        return -1;
    }
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(wide_pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        count++;
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
#else
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        size_t name_length = strlen(entry->d_name);
        if (name_length >= 4 && strcmp(entry->d_name + name_length - 4, ".tmp") == 0) {
            count++;
        }
    }
    closedir(handle);
#endif
    return count;
}

static void read_back_equals(const char *path, const char *expected) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    CHECK(bytes != NULL);
    if (bytes != NULL) {
        CHECK(length == strlen(expected));
        check_text((const char *)bytes, expected, path);
    }
    free(bytes);
}

static void make_directories_creates_nested(void) {
    const char *nested = SPISOR_SCRATCH_PATH "/a/b/c\xc3\xa9";
    CHECK(spisor_make_directories(nested) == 1);
    CHECK(spisor_file_exists(nested) != 0);
    CHECK(spisor_make_directories(nested) == 1);
}

static void append_creates_then_appends(void) {
    const char *path = SPISOR_SCRATCH_PATH "/appended.log";
    remove(path);
    CHECK(spisor_append_file(path, (const uint8_t *)"x\n", 2) == 1);
    CHECK(spisor_append_file(path, (const uint8_t *)"y\n", 2) == 1);
    read_back_equals(path, "x\ny\n");
}

static void replace_overwrites_existing(void) {
    const char *directory = SPISOR_SCRATCH_PATH "/replace";
    const char *path = SPISOR_SCRATCH_PATH "/replace/settings.json";
    CHECK(spisor_make_directories(directory) == 1);
    CHECK(spisor_replace_file(path, (const uint8_t *)"old", 3) == 1);
    CHECK(spisor_replace_file(path, (const uint8_t *)"new", 3) == 1);
    read_back_equals(path, "new");
    CHECK(count_temporary_entries(directory) == 0);
}

static void replace_into_missing_directory_fails(void) {
    const char *path = SPISOR_SCRATCH_PATH "/no-such-directory/settings.json";
    CHECK(spisor_replace_file(path, (const uint8_t *)"new", 3) == 0);
    CHECK(spisor_file_exists(SPISOR_SCRATCH_PATH "/no-such-directory") == 0);
}

int main(void) {
    a_directory_exists();
    a_missing_file_does_not();
    a_directory_is_not_readable();
    a_non_ascii_name_round_trips();
    an_empty_file_reads_as_empty();
    make_directories_creates_nested();
    append_creates_then_appends();
    replace_overwrites_existing();
    replace_into_missing_directory_fails();
    return check_summary("file_test");
}
