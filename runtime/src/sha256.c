#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_stream.h"

#define FILE_CHUNK 65536

static const uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static const uint32_t initial_state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

static uint32_t big_endian_word(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void expand_schedule(const uint8_t *block, uint32_t schedule[64]) {
    for (unsigned index = 0; index < 16; index++) {
        schedule[index] = big_endian_word(block + index * 4);
    }
    for (unsigned index = 16; index < 64; index++) {
        uint32_t early = schedule[index - 15];
        uint32_t late = schedule[index - 2];
        uint32_t sigma0 = rotate_right(early, 7) ^ rotate_right(early, 18) ^ (early >> 3);
        uint32_t sigma1 = rotate_right(late, 17) ^ rotate_right(late, 19) ^ (late >> 10);
        schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
    }
}

static void compress(uint32_t state[8], const uint8_t *block) {
    uint32_t schedule[64];
    expand_schedule(block, schedule);
    uint32_t working[8];
    memcpy(working, state, sizeof working);
    for (unsigned index = 0; index < 64; index++) {
        uint32_t e = working[4];
        uint32_t a = working[0];
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & working[5]) ^ (~e & working[6]);
        uint32_t first = working[7] + sum1 + choice + round_constants[index] + schedule[index];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & working[1]) ^ (a & working[2]) ^ (working[1] & working[2]);
        memmove(working + 1, working, 7 * sizeof working[0]);
        working[4] += first;
        working[0] = first + sum0 + majority;
    }
    for (unsigned index = 0; index < 8; index++) {
        state[index] += working[index];
    }
}

void runtime_sha256_start(runtime_sha256 *hash) {
    memcpy(hash->state, initial_state, sizeof initial_state);
    hash->length = 0;
    hash->filled = 0;
}

void runtime_sha256_add(runtime_sha256 *hash, const uint8_t *bytes, size_t length) {
    hash->length += length;
    while (length > 0) {
        size_t taken = sizeof hash->block - hash->filled;
        taken = taken < length ? taken : length;
        memcpy(hash->block + hash->filled, bytes, taken);
        hash->filled += taken;
        bytes += taken;
        length -= taken;
        if (hash->filled == sizeof hash->block) {
            compress(hash->state, hash->block);
            hash->filled = 0;
        }
    }
}

static void pad(runtime_sha256 *hash) {
    uint64_t bit_length = hash->length * 8;
    hash->block[hash->filled++] = 0x80;
    if (hash->filled > 56) {
        memset(hash->block + hash->filled, 0, sizeof hash->block - hash->filled);
        compress(hash->state, hash->block);
        hash->filled = 0;
    }
    memset(hash->block + hash->filled, 0, 56 - hash->filled);
    for (unsigned index = 0; index < 8; index++) {
        hash->block[63 - index] = (uint8_t)(bit_length >> (index * 8));
    }
    compress(hash->state, hash->block);
}

void runtime_sha256_finish_hex(runtime_sha256 *hash, char out_hex[RUNTIME_SHA256_HEX_SIZE]) {
    static const char digits[] = "0123456789abcdef";
    pad(hash);
    for (unsigned index = 0; index < 32; index++) {
        uint8_t byte = (uint8_t)(hash->state[index / 4] >> (24 - (index % 4) * 8));
        out_hex[index * 2] = digits[byte >> 4];
        out_hex[index * 2 + 1] = digits[byte & 0x0f];
    }
    out_hex[64] = 0;
}

int runtime_sha256_file_sized(const char *path, char out_hex[RUNTIME_SHA256_HEX_SIZE], uint64_t *size) {
    FILE *file = runtime_open_file(path, RUNTIME_FILE_READ);
    uint8_t *chunk = malloc(FILE_CHUNK);
    if (file == NULL || chunk == NULL) {
        if (file != NULL) {
            fclose(file);
        }
        free(chunk);
        return 0;
    }
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    uint64_t total = 0;
    size_t read;
    while ((read = fread(chunk, 1, FILE_CHUNK, file)) > 0) {
        runtime_sha256_add(&hash, chunk, read);
        total += read;
    }
    int complete = !ferror(file);
    fclose(file);
    free(chunk);
    if (complete) {
        runtime_sha256_finish_hex(&hash, out_hex);
        *size = total;
    }
    return complete;
}

int spisor_sha256_file(const char *path, char out_hex[RUNTIME_SHA256_HEX_SIZE]) {
    uint64_t size;
    return runtime_sha256_file_sized(path, out_hex, &size);
}
