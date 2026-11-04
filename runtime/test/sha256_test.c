#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

static void expect_digest(const char *message, const char *expected, const char *label) {
    runtime_sha256 hash;
    char hex[RUNTIME_SHA256_HEX_SIZE];
    runtime_sha256_start(&hash);
    runtime_sha256_add(&hash, (const uint8_t *)message, strlen(message));
    runtime_sha256_finish_hex(&hash, hex);
    check_text(hex, expected, label);
}

static void the_empty_message(void) {
    expect_digest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
}

static void the_abc_message(void) {
    expect_digest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
}

static void the_448_bit_message(void) {
    expect_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448-bit");
}

static void a_million_a_fed_in_uneven_pieces(void) {
    uint8_t piece[997];
    memset(piece, 'a', sizeof piece);
    runtime_sha256 hash;
    runtime_sha256_start(&hash);
    size_t remaining = 1000000;
    while (remaining > 0) {
        size_t length = remaining < sizeof piece ? remaining : sizeof piece;
        runtime_sha256_add(&hash, piece, length);
        remaining -= length;
    }
    char hex[RUNTIME_SHA256_HEX_SIZE];
    runtime_sha256_finish_hex(&hash, hex);
    check_text(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "million a");
}

static void a_file_is_hashed_whole_and_a_missing_one_is_refused(void) {
    char directory[1024];
    char path[1100];
    snprintf(directory, sizeof directory, "%s/sha256-J\xc3\xb6rg-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(directory);
    snprintf(path, sizeof path, "%s/abc.txt", directory);
    CHECK(spisor_replace_file(path, (const uint8_t *)"abc", 3) == 1);
    char hex[RUNTIME_SHA256_HEX_SIZE] = "";
    CHECK(spisor_sha256_file(path, hex) == 1);
    check_text(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "file abc");
    snprintf(path, sizeof path, "%s/absent.txt", directory);
    CHECK(spisor_sha256_file(path, hex) == 0);
    CHECK(runtime_remove_tree(directory) == 1);
}

int main(void) {
    the_empty_message();
    the_abc_message();
    the_448_bit_message();
    a_million_a_fed_in_uneven_pieces();
    a_file_is_hashed_whole_and_a_missing_one_is_refused();
    return check_summary("sha256_test");
}
