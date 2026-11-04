#ifndef RUNTIME_SHA256_H
#define RUNTIME_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define RUNTIME_SHA256_HEX_SIZE 65

typedef struct runtime_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t filled;
} runtime_sha256;

void runtime_sha256_start(runtime_sha256 *hash);

void runtime_sha256_add(runtime_sha256 *hash, const uint8_t *bytes, size_t length);

/** Ends the hash and writes its digest as 64 lowercase hex digits and a NUL. */
void runtime_sha256_finish_hex(runtime_sha256 *hash, char out_hex[RUNTIME_SHA256_HEX_SIZE]);

/** The SHA-256 of the whole file at path, as lowercase hex. 1 on success, 0 when it cannot be read. */
int spisor_sha256_file(const char *path, char out_hex[RUNTIME_SHA256_HEX_SIZE]);

#endif
