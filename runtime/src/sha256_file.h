#ifndef RUNTIME_SHA256_FILE_H
#define RUNTIME_SHA256_FILE_H

#include <stdint.h>

#include "naula/sha256.h"
#include "spisor/status.h"

/**
 * The SHA-256 of the whole file at path, as lowercase hex: SPISOR_REFUSED when it cannot be read, SPISOR_OUT_OF_MEMORY
 * when memory runs out.
 */
spisor_status spisor_sha256_file(const char *path, char out_hex[NAULA_SHA256_HEX_SIZE]);

/** spisor_sha256_file that also answers the file's length in bytes into *size. */
spisor_status runtime_sha256_file_sized(const char *path, char out_hex[NAULA_SHA256_HEX_SIZE], uint64_t *size);

#endif
