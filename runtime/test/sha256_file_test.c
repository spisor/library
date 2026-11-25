#include <stdint.h>
#include <stdio.h>

#include "check.h"
#include "scratch_directory.h"
#include "sha256_file.h"
#include "spisor/file.h"

#if defined(_WIN32)
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

static void a_file_is_hashed_whole_and_a_missing_one_is_refused(void) {
    char directory[1024];
    char path[1100];
    snprintf(directory, sizeof directory, "%s/sha256-J\xc3\xb6rg-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(directory);
    snprintf(path, sizeof path, "%s/abc.txt", directory);
    CHECK(spisor_replace_file(path, (const uint8_t *)"abc", 3) == SPISOR_OK);
    char hex[NAULA_SHA256_HEX_SIZE] = "";
    CHECK(spisor_sha256_file(path, hex) == SPISOR_OK);
    check_text(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "file abc");
    snprintf(path, sizeof path, "%s/absent.txt", directory);
    CHECK(spisor_sha256_file(path, hex) == SPISOR_REFUSED);
    CHECK(runtime_remove_tree(directory) == SPISOR_OK);
}

int main(void) {
    a_file_is_hashed_whole_and_a_missing_one_is_refused();
    return check_summary("sha256_file_test");
}
