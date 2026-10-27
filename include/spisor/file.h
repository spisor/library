#ifndef SPISOR_FILE_H
#define SPISOR_FILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Node's existsSync: true for a file or a directory.
 *
 * @implNote Paths are UTF-8 everywhere; on Windows they are converted to UTF-16 for the wide
 * APIs, so a non-ASCII name resolves whatever the process code page is.
 */
int spisor_file_exists(const char *path);

/**
 * The whole file, freed with free, or NULL when it cannot be read. The buffer carries one NUL
 * past length so a text file can be handed to a string parser directly.
 */
uint8_t *spisor_read_file(const char *path, size_t *length);

/** Creates path and every missing parent. 1 on success, including when path already exists. */
int spisor_make_directories(const char *path);

/** Appends length bytes, creating the file when absent. 1 on success. */
int spisor_append_file(const char *path, const uint8_t *bytes, size_t length);

/**
 * Writes length bytes so a reader sees the old content or the new, never a mix. 1 on success.
 *
 * @implNote The bytes go to a temporary sibling named after path with a random hex suffix and a
 * .tmp extension, which is renamed over path; it is removed again on any failure. The directory
 * must already exist.
 */
int spisor_replace_file(const char *path, const uint8_t *bytes, size_t length);

#ifdef __cplusplus
}
#endif

#endif
