#ifndef SPISOR_TEST_LINKS_H
#define SPISOR_TEST_LINKS_H

#include <stdio.h>
#include <stdlib.h>

#include "check.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define LINK_PRIVILEGE_MISSING 2
#define SKIPPED_EXIT_CODE 77

#if defined(_WIN32)
static inline int is_privilege_refusal(DWORD error) {
    return error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_INVALID_PARAMETER;
}

static inline int create_link(const char *link_path, const char *target_path) {
    wchar_t wide_link[1024];
    wchar_t wide_target[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, link_path, -1, wide_link, 1024) == 0
        || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target_path, -1, wide_target, 1024) == 0) {
        return 0;
    }
    for (wchar_t *cursor = wide_target; *cursor != 0; cursor++) {
        *cursor = *cursor == L'/' ? L'\\' : *cursor;
    }
    DeleteFileW(wide_link);
    if (CreateSymbolicLinkW(wide_link, wide_target, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0) {
        return 1;
    }
    return is_privilege_refusal(GetLastError()) ? LINK_PRIVILEGE_MISSING : 0;
}

static inline int is_link(const char *path) {
    wchar_t wide_path[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, 1024) == 0) {
        return 0;
    }
    DWORD attributes = GetFileAttributesW(wide_path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
static inline int create_link(const char *link_path, const char *target_path) {
    remove(link_path);
    return symlink(target_path, link_path) == 0;
}

static inline int is_link(const char *path) {
    struct stat status;
    return lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
}
#endif

static int links_skipped = 0;

/**
 * Creates the link, or on a machine that refuses links without privilege records a skip and answers 0.
 *
 * @implNote CI must never skip, so there a refusal is a failure: a dotfiles symlink is a case every
 * runner has to prove.
 */
static inline int create_link_or_skip(const char *link_path, const char *target_path) {
    int created = create_link(link_path, target_path);
    if (created == LINK_PRIVILEGE_MISSING && getenv("CI") == NULL) {
        links_skipped = 1;
        printf("SKIP symlink tests: this platform refuses to create symlinks without privilege\n");
        return 0;
    }
    CHECK(created == 1);
    return created == 1;
}

/** The exit code of a suite: a skip only when every check that ran passed and a link was refused. */
static inline int summary_with_links(const char *suite) {
    int failed = check_summary(suite);
    return failed == 0 && links_skipped ? SKIPPED_EXIT_CODE : failed;
}

#endif
