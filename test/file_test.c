#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "spisor/file.h"
#include "../src/file_presence.h"
#include "../src/file_replacement.h"
#include "links.h"
#include "temporary_entries.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
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

static void replace_through_a_link_changes_the_target_and_keeps_the_link(void) {
    const char *real_directory = SPISOR_SCRATCH_PATH "/link-real";
    const char *home_directory = SPISOR_SCRATCH_PATH "/link-home";
    const char *target = SPISOR_SCRATCH_PATH "/link-real/settings.json";
    const char *link = SPISOR_SCRATCH_PATH "/link-home/settings.json";
    CHECK(spisor_make_directories(real_directory) == 1);
    CHECK(spisor_make_directories(home_directory) == 1);
    CHECK(write_fixture(target, "old"));
    if (!create_link_or_skip(link, target)) {
        return;
    }
    CHECK(spisor_replace_file(link, (const uint8_t *)"new", 3) == 1);
    CHECK(is_link(link));
    read_back_equals(target, "new");
    read_back_equals(link, "new");
    CHECK(spisor_probe_file(link) == SPISOR_FILE_PRESENT);
    CHECK(count_temporary_entries(real_directory) == 0);
    CHECK(count_temporary_entries(home_directory) == 0);
}

static void replace_through_a_dangling_link_is_refused(void) {
    const char *home_directory = SPISOR_SCRATCH_PATH "/dangling-home";
    const char *link = SPISOR_SCRATCH_PATH "/dangling-home/settings.json";
    CHECK(spisor_make_directories(home_directory) == 1);
    if (!create_link_or_skip(link, "no-such-target.json")) {
        return;
    }
    CHECK(spisor_replace_file(link, (const uint8_t *)"new", 3) == 0);
    CHECK(is_link(link));
    CHECK(spisor_file_exists(SPISOR_SCRATCH_PATH "/dangling-home/no-such-target.json") == 0);
    CHECK(count_temporary_entries(home_directory) == 0);
}

static void a_private_replace_never_writes_through_a_link(void) {
    const char *real_directory = SPISOR_SCRATCH_PATH "/private-link-real";
    const char *home_directory = SPISOR_SCRATCH_PATH "/private-link-home";
    const char *target = SPISOR_SCRATCH_PATH "/private-link-real/secrets.json";
    const char *link = SPISOR_SCRATCH_PATH "/private-link-home/secrets.json";
    CHECK(spisor_make_directories(real_directory) == 1);
    CHECK(spisor_make_directories(home_directory) == 1);
    CHECK(write_fixture(target, "old"));
    CHECK(spisor_is_link(target) == 0);
    if (!create_link_or_skip(link, target)) {
        return;
    }
    CHECK(spisor_is_link(link) == 1);
    CHECK(spisor_replace_private_file(link, (const uint8_t *)"new", 3) == 0);
    CHECK(is_link(link));
    read_back_equals(target, "old");
    CHECK(count_temporary_entries(real_directory) == 0);
    CHECK(count_temporary_entries(home_directory) == 0);
}

#if !defined(_WIN32)
static int mode_of(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 ? (int)(status.st_mode & 07777) : -1;
}

static void replace_keeps_the_permission_bits(void) {
    const char *directory = SPISOR_SCRATCH_PATH "/modes";
    const char *target = SPISOR_SCRATCH_PATH "/modes/secrets.json";
    const char *link = SPISOR_SCRATCH_PATH "/modes/secrets-link.json";
    CHECK(spisor_make_directories(directory) == 1);
    CHECK(write_fixture(target, "old"));
    CHECK(chmod(target, 0600) == 0);
    CHECK(spisor_replace_file(target, (const uint8_t *)"direct", 6) == 1);
    CHECK(mode_of(target) == 0600);
    CHECK(create_link(link, target) == 1);
    CHECK(spisor_replace_file(link, (const uint8_t *)"linked", 6) == 1);
    CHECK(mode_of(target) == 0600);
    read_back_equals(target, "linked");
}

static void private_replace_creates_and_keeps_an_owner_only_file(void) {
    const char *directory = SPISOR_SCRATCH_PATH "/private";
    const char *path = SPISOR_SCRATCH_PATH "/private/secrets.json";
    CHECK(spisor_make_directories(directory) == 1);
    remove(path);
    mode_t previous = umask(0);
    CHECK(spisor_replace_private_file(path, (const uint8_t *)"first", 5) == 1);
    CHECK(mode_of(path) == 0600);
    CHECK(chmod(path, 0644) == 0);
    CHECK(spisor_replace_private_file(path, (const uint8_t *)"second", 6) == 1);
    CHECK(mode_of(path) == 0600);
    umask(previous);
    read_back_equals(path, "second");
    CHECK(count_temporary_entries(directory) == 0);
}

static int replacement_mode(const char *target, int owner_only) {
    char *temporary = NULL;
    FILE *file = spisor_open_replacement(target, owner_only, &temporary);
    if (file == NULL) {
        return -1;
    }
    int mode = mode_of(temporary);
    fclose(file);
    remove(temporary);
    free(temporary);
    return mode;
}

static void a_replacement_carries_its_final_mode_before_any_byte_is_written(void) {
    const char *directory = SPISOR_SCRATCH_PATH "/replacement-modes";
    const char *existing = SPISOR_SCRATCH_PATH "/replacement-modes/existing.json";
    const char *absent = SPISOR_SCRATCH_PATH "/replacement-modes/absent.json";
    CHECK(spisor_make_directories(directory) == 1);
    remove(absent);
    CHECK(write_fixture(existing, "old"));
    CHECK(chmod(existing, 0640) == 0);
    mode_t previous = umask(0);
    CHECK(replacement_mode(existing, 0) == 0640);
    CHECK(replacement_mode(existing, 1) == 0600);
    CHECK(replacement_mode(absent, 1) == 0600);
    CHECK(replacement_mode(absent, 0) == 0666);
    umask(022);
    CHECK(replacement_mode(absent, 0) == 0644);
    umask(previous);
    CHECK(count_temporary_entries(directory) == 0);
}
#endif

static void private_replace_writes_the_bytes(void) {
    const char *directory = SPISOR_SCRATCH_PATH "/private-bytes";
    const char *path = SPISOR_SCRATCH_PATH "/private-bytes/secrets.json";
    CHECK(spisor_make_directories(directory) == 1);
    CHECK(spisor_replace_private_file(path, (const uint8_t *)"old", 3) == 1);
    CHECK(spisor_replace_private_file(path, (const uint8_t *)"new", 3) == 1);
    read_back_equals(path, "new");
    CHECK(count_temporary_entries(directory) == 0);
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
    replace_through_a_link_changes_the_target_and_keeps_the_link();
    replace_through_a_dangling_link_is_refused();
    a_private_replace_never_writes_through_a_link();
#if !defined(_WIN32)
    replace_keeps_the_permission_bits();
    private_replace_creates_and_keeps_an_owner_only_file();
    a_replacement_carries_its_final_mode_before_any_byte_is_written();
#endif
    private_replace_writes_the_bytes();
    return summary_with_links("file_test");
}
