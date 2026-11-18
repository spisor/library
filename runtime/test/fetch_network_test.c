#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "runtime/fetch.h"
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

#define PINNED_URL "https://raw.githubusercontent.com/DaveGamble/cJSON/acc76239bee01d8e9c858ae2cab296704e52d916/LICENSE"
#define PINNED_SHA256 "a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c"
#define PINNED_BYTES 1084

static void a_pinned_https_file_is_fetched_over_the_os_stack(void) {
    const runtime_fetch_definition *fetch = runtime_fetch_definition_of();
    runtime_fetch_get_argument argument = SPISOR_INIT(runtime_fetch_get_argument);
    runtime_fetch_get_answer answer = SPISOR_INIT(runtime_fetch_get_answer);
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    char scratch[1024];
    char destination[1100];
    snprintf(scratch, sizeof scratch, "%s/fetch-network-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(scratch);
    snprintf(destination, sizeof destination, "%s/LICENSE", scratch);
    argument.url = PINNED_URL;
    argument.destination = destination;
    argument.sha256 = PINNED_SHA256;
    spisor_status status = fetch->get(fetch->context, &argument, &answer, &refusal);
    if (status != SPISOR_OK) {
        fprintf(stderr, "refused: %s; %s\n", refusal.detail, refusal.fix);
    }
    CHECK(status == SPISOR_OK);
    CHECK(answer.outcome == RUNTIME_FETCH_GET_STORED);
    check_text(answer.sha256, PINNED_SHA256, "sha256");
    CHECK(answer.bytes == PINNED_BYTES);
    fetch->release_answer(fetch->context, &answer);
    fetch->release_refusal(fetch->context, &refusal);
    char stored[RUNTIME_SHA256_HEX_SIZE] = "";
    CHECK(spisor_sha256_file(destination, stored) == SPISOR_OK);
    check_text(stored, PINNED_SHA256, "stored file");
    CHECK(runtime_remove_tree(scratch) == SPISOR_OK);
}

int main(void) {
    a_pinned_https_file_is_fetched_over_the_os_stack();
    return check_summary("fetch_network_test");
}
