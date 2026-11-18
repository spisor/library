#ifndef SPISOR_FILE_H
#define SPISOR_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "spisor/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum spisor_file_presence {
    SPISOR_FILE_PRESENT,
    SPISOR_FILE_MISSING,
    SPISOR_FILE_UNKNOWN,
    SPISOR_FILE_OUT_OF_MEMORY
} spisor_file_presence;

/**
 * Whether something, a file or a directory, exists at path, telling a path that genuinely does not exist
 * (MISSING) apart from one the OS would not answer for (UNKNOWN), such as a parent without search
 * permission, and both from a question memory ran out for (OUT_OF_MEMORY). Node's existsSync is PRESENT.
 *
 * @implNote MISSING is only POSIX ENOENT or ENOTDIR, or Windows ERROR_FILE_NOT_FOUND or
 * ERROR_PATH_NOT_FOUND. A caller about to replace a file must treat UNKNOWN as present, so it never
 * replaces a file it failed to see. Paths are UTF-8 everywhere; on Windows they are converted to UTF-16
 * for the wide APIs, so a non-ASCII name resolves whatever the process code page is.
 */
spisor_file_presence spisor_probe_file(const char *path);

/**
 * The whole file, freed, or NULL when it cannot be read. The buffer carries one NUL
 * past length so a text file can be handed to a string parser directly.
 */
uint8_t *spisor_read_file(const char *path, size_t *length);

/** Creates path and every missing parent. 1 on success, including when path already exists. */
int spisor_make_directories(const char *path);

/** Appends length bytes, creating the file when absent. 1 on success. */
int spisor_append_file(const char *path, const uint8_t *bytes, size_t length);

/**
 * Writes length bytes so a reader sees the old content or the new, never a mix: SPISOR_REFUSED when it
 * cannot be written, SPISOR_OUT_OF_MEMORY when memory runs out, the file unchanged for both.
 *
 * @implNote The bytes go to a temporary sibling named after path with a random hex suffix and a
 * .tmp extension, which is renamed over path; it is removed again on any failure. The directory
 * must already exist. On POSIX the temporary file is created already carrying the replaced file's
 * mode (a new file gets the umask's default), so a rewrite never passes through a wider mode.
 *
 * A path that is a symlink is resolved to its final target first, and the temporary file is
 * created beside that target, so the link survives and the target changes. This keeps a home
 * directory managed by stow or chezmoi intact. A dangling link is refused. Reads and
 * spisor_probe_file already follow links.
 */
spisor_status spisor_replace_file(const char *path, const uint8_t *bytes, size_t length);

/**
 * Deletes what spisor_replace_file set aside to replace the running Windows executable at path, answering
 * how many such files are left because they still run. Always 0 elsewhere, where nothing is ever set aside.
 */
int spisor_remove_set_aside(const char *path);

/**
 * Whether path itself is a symbolic link, dangling or not; on Windows, any reparse point, which is
 * what spisor_replace_file follows.
 */
int spisor_is_link(const char *path);

/**
 * spisor_replace_file for a file only its owner may read, such as a secrets file. On POSIX the
 * result is mode 0600 whether the file is new or replaced, and the temporary file carries 0600 from
 * the moment it is created, so the bytes are never readable by anyone else, not even briefly.
 * Unlike spisor_replace_file, a path that is a symbolic link is refused rather than
 * followed: a link's target may sit somewhere its owner shares, such as a dotfiles repository.
 *
 * @implNote Windows has no mode: the file takes the ACL its directory passes on. Under
 * %LOCALAPPDATA%, where the secrets file lives by default, that ACL already admits only the user,
 * SYSTEM and administrators, so nothing is narrowed here. A directory named some other way (such as
 * SPISOR_SECRETS) keeps whatever ACL its owner gave it.
 */
spisor_status spisor_replace_private_file(const char *path, const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
