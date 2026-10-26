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

#ifdef __cplusplus
}
#endif

#endif
