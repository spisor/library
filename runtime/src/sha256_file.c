#include "sha256_file.h"
#include "allocation.h"

#include <stdio.h>

#include "file_stream.h"

#define FILE_CHUNK 65536

spisor_status runtime_sha256_file_sized(const char *path, char out_hex[NAULA_SHA256_HEX_SIZE], uint64_t *size) {
    int out_of_memory;
    FILE *file = runtime_open_file(path, RUNTIME_FILE_READ, &out_of_memory);
    uint8_t *chunk = file == NULL ? NULL : spisor_memory_allocate(FILE_CHUNK);
    if (chunk == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        return file != NULL || out_of_memory ? SPISOR_OUT_OF_MEMORY : SPISOR_REFUSED;
    }
    naula_sha256 hash;
    naula_sha256_start(&hash);
    uint64_t total = 0;
    size_t read;
    while ((read = fread(chunk, 1, FILE_CHUNK, file)) > 0) {
        naula_sha256_add(&hash, chunk, read);
        total += read;
    }
    int complete = !ferror(file);
    fclose(file);
    spisor_memory_release(chunk);
    if (complete) {
        naula_sha256_hex(&hash, out_hex);
        *size = total;
    }
    return complete ? SPISOR_OK : SPISOR_REFUSED;
}

spisor_status spisor_sha256_file(const char *path, char out_hex[NAULA_SHA256_HEX_SIZE]) {
    uint64_t size;
    return runtime_sha256_file_sized(path, out_hex, &size);
}
