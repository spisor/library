#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "fetch.h"
#include "scratch_directory.h"
#include "sha256.h"
#include "spisor/file.h"
#include "spisor/json_kind.h"

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

static const char *text_of(const cJSON *tree, const char *member) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(tree, member);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void a_pinned_https_file_is_fetched_over_the_os_stack(void) {
    char scratch[1024];
    char destination[1100];
    snprintf(scratch, sizeof scratch, "%s/fetch-network-%d", RUNTIME_SCRATCH_PATH, (int)current_process_id());
    start_empty_directory(scratch);
    snprintf(destination, sizeof destination, "%s/LICENSE", scratch);
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "url", PINNED_URL);
    cJSON_AddStringToObject(request, "destination", destination);
    cJSON_AddStringToObject(request, "sha256", PINNED_SHA256);
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    uint8_t *out = NULL;
    size_t out_len = 0;
    int32_t status = runtime_fetch_get((const uint8_t *)text, strlen(text), &out, &out_len);
    free(text);
    cJSON *tree = out == NULL ? NULL : spisor_parse_json(out, out_len);
    free(out);
    if (status != SPISOR_STATUS_SUCCESS) {
        fprintf(stderr, "refused: %s; %s\n", text_of(tree, "message"), text_of(tree, "fix"));
    }
    CHECK(status == SPISOR_STATUS_SUCCESS);
    check_text(text_of(tree, "outcome"), "stored", "outcome");
    check_text(text_of(tree, "sha256"), PINNED_SHA256, "sha256");
    const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(tree, "bytes");
    CHECK(cJSON_IsNumber(bytes) && bytes->valuedouble == PINNED_BYTES);
    cJSON_Delete(tree);
    char stored[RUNTIME_SHA256_HEX_SIZE] = "";
    CHECK(spisor_sha256_file(destination, stored) == 1);
    check_text(stored, PINNED_SHA256, "stored file");
    CHECK(runtime_remove_tree(scratch) == 1);
}

int main(void) {
    a_pinned_https_file_is_fetched_over_the_os_stack();
    return check_summary("fetch_network_test");
}
