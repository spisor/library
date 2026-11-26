#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_call.h"
#include "spisor/allocation.h"
#include "check.h"
#include "marketplace_call.h"
#include "marketplace_memory.h"
#include "service_scratch.h"
#include "spisor/file.h"

static char scratch[1024];

static int is_checking_only;

static catalog_answer remember(const char *marketplace, const char *name, const char *id, const char *accept) {
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", scratch);
    if (marketplace != NULL) {
        cJSON_AddStringToObject(argument, "marketplace", marketplace);
    }
    if (is_checking_only) {
        cJSON_AddFalseToObject(argument, "record");
    }
    cJSON_AddStringToObject(argument, "name", name);
    cJSON_AddStringToObject(argument, "id", id);
    if (accept != NULL) {
        cJSON_AddStringToObject(argument, "accept", accept);
    }
    return marketplace_call(MARKETPLACE_REMEMBER, argument);
}

static char *store_text(void) {
    char *path = joined_path(scratch, "marketplace-names.json");
    size_t length = 0;
    char *text = (char *)spisor_read_file_reporting(path, &length, &(spisor_read_outcome){SPISOR_READ_FAILED});
    spisor_memory_release(path);
    return text;
}

static int is_remembered(const catalog_answer answer) {
    return answer.status == SPISOR_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "remembered"));
}

static void remembers_a_first_install(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "first");
    catalog_answer answer = remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL);
    CHECK(is_remembered(answer));
    char *text = store_text();
    cJSON *store = text == NULL ? NULL : parsed(text);
    CHECK(store != NULL);
    check_text(text_at(value_at(store, "names"), "spisor/runtime"), "git:0000000000000000000000000000000000000001", "the recorded id");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(store, "format")) == 1);
    cJSON_Delete(store);
    spisor_memory_release(text);
    cJSON_Delete(answer.tree);
}

static void the_same_id_again_is_fine(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "same");
    cJSON_Delete(remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    catalog_answer again = remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL);
    CHECK(is_remembered(again));
    cJSON_Delete(again.tree);
}

static void one_name_in_two_marketplaces_is_two_memories(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "two");
    cJSON_Delete(remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    catalog_answer other = remember("other", "runtime", "git:0000000000000000000000000000000000000009", NULL);
    CHECK(is_remembered(other));
    char *text = store_text();
    CHECK(text != NULL && strstr(text, "spisor/runtime") != NULL && strstr(text, "other/runtime") != NULL);
    spisor_memory_release(text);
    cJSON_Delete(other.tree);
}

static void a_different_id_for_a_remembered_name_needs_the_user_naming_both(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "different");
    cJSON_Delete(remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    catalog_answer answer = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", NULL);
    CHECK(answer.status == SPISOR_OK);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "needsUserAcceptance")));
    CHECK(cJSON_GetObjectItemCaseSensitive(answer.tree, "remembered") == NULL);
    check_text(text_at(answer.tree, "was"), "git:0000000000000000000000000000000000000001", "the remembered id");
    check_text(text_at(answer.tree, "now"), "git:0000000000000000000000000000000000000002", "the offered id");
    char *text = store_text();
    CHECK(text != NULL && strstr(text, "git:0000000000000000000000000000000000000001") != NULL && strstr(text, "git:0000000000000000000000000000000000000002") == NULL);
    spisor_memory_release(text);
    catalog_answer wrong = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", "git:0000000000000000000000000000000000000003");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wrong.tree, "needsUserAcceptance")));
    cJSON_Delete(wrong.tree);
    cJSON_Delete(answer.tree);
}

static void accept_records_the_new_id(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "accept");
    cJSON_Delete(remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    catalog_answer accepted = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", "git:0000000000000000000000000000000000000002");
    CHECK(is_remembered(accepted));
    char *text = store_text();
    CHECK(text != NULL && strstr(text, "git:0000000000000000000000000000000000000002") != NULL && strstr(text, "git:0000000000000000000000000000000000000001") == NULL);
    spisor_memory_release(text);
    catalog_answer after = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", NULL);
    CHECK(is_remembered(after));
    cJSON_Delete(after.tree);
    cJSON_Delete(accepted.tree);
}

static void write_store(const char *text) {
    char *path = joined_path(scratch, "marketplace-names.json");
    CHECK(spisor_replace_file(path, (const uint8_t *)text, strlen(text)) == SPISOR_OK);
    spisor_memory_release(path);
}

static void a_corrupt_store_is_refused_not_overwritten(void) {
    const char *corrupt[] = {"{\"format\":1", "[]", "{\"format\":2,\"names\":{}}", "{\"format\":1,\"names\":{\"spisor/runtime\":5}}"};
    for (size_t index = 0; index < sizeof corrupt / sizeof corrupt[0]; index++) {
        scratch_in(scratch, sizeof scratch, "marketplace-memory", "corrupt");
        write_store(corrupt[index]);
        catalog_answer answer = remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL);
        CHECK(expect_refusal(answer, "marketplace-names.json", "a corrupt store"));
        CHECK(marketplace_last_status == SPISOR_REFUSED);
        char *text = store_text();
        check_text(text, corrupt[index], "the store is left as it was");
        spisor_memory_release(text);
        cJSON_Delete(answer.tree);
    }
}

static void refuses_an_argument_without_what_it_needs(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "arguments");
    cJSON *argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", scratch);
    expect_refusal_released(marketplace_call(MARKETPLACE_REMEMBER, argument), "name", "no name");
    CHECK(marketplace_last_status == SPISOR_MALFORMED);
    argument = cJSON_CreateObject();
    cJSON_AddStringToObject(argument, "state", "relative/state");
    cJSON_AddStringToObject(argument, "marketplace", "spisor");
    cJSON_AddStringToObject(argument, "name", "runtime");
    cJSON_AddStringToObject(argument, "id", "git:0000000000000000000000000000000000000001");
    expect_refusal_released(marketplace_call(MARKETPLACE_REMEMBER, argument), "state", "a relative state");
    CHECK(marketplace_last_status == SPISOR_MALFORMED);
}

static int is_settled(const catalog_answer answer) {
    return answer.status == SPISOR_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(answer.tree, "settled"));
}

static void a_check_writes_nothing_and_still_finds_a_change(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "check");
    is_checking_only = 1;
    catalog_answer first = remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL);
    CHECK(is_settled(first));
    char *text = store_text();
    CHECK(text == NULL);
    is_checking_only = 0;
    cJSON_Delete(remember("spisor", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    is_checking_only = 1;
    catalog_answer changed = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", NULL);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(changed.tree, "needsUserAcceptance")));
    catalog_answer accepted = remember("spisor", "runtime", "git:0000000000000000000000000000000000000002", "git:0000000000000000000000000000000000000002");
    CHECK(is_settled(accepted));
    text = store_text();
    CHECK(text != NULL && strstr(text, "git:0000000000000000000000000000000000000001") != NULL && strstr(text, "git:0000000000000000000000000000000000000002") == NULL);
    is_checking_only = 0;
    spisor_memory_release(text);
    cJSON_Delete(accepted.tree);
    cJSON_Delete(changed.tree);
    cJSON_Delete(first.tree);
}

static void a_bare_name_is_remembered_beside_the_qualified_ones(void) {
    scratch_in(scratch, sizeof scratch, "marketplace-memory", "bare");
    cJSON_Delete(remember("first", "runtime", "git:0000000000000000000000000000000000000001", NULL).tree);
    catalog_answer bare = remember(NULL, "runtime", "git:0000000000000000000000000000000000000001", NULL);
    CHECK(is_remembered(bare));
    char *text = store_text();
    cJSON *store = parsed(text);
    check_text(text_at(value_at(store, "names"), "runtime"), "git:0000000000000000000000000000000000000001", "the bare key");
    check_text(text_at(value_at(store, "names"), "first/runtime"), "git:0000000000000000000000000000000000000001", "the qualified key");
    cJSON_Delete(store);
    spisor_memory_release(text);
    catalog_answer moved = remember(NULL, "runtime", "git:0000000000000000000000000000000000000002", NULL);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(moved.tree, "needsUserAcceptance")));
    check_text(text_at(moved.tree, "was"), "git:0000000000000000000000000000000000000001", "the bare name's remembered id");
    cJSON_Delete(moved.tree);
    cJSON_Delete(bare.tree);
}

int main(void) {
    remembers_a_first_install();
    the_same_id_again_is_fine();
    one_name_in_two_marketplaces_is_two_memories();
    a_different_id_for_a_remembered_name_needs_the_user_naming_both();
    accept_records_the_new_id();
    a_corrupt_store_is_refused_not_overwritten();
    refuses_an_argument_without_what_it_needs();
    a_check_writes_nothing_and_still_finds_a_change();
    a_bare_name_is_remembered_beside_the_qualified_ones();
    return check_summary("marketplace_memory_test");
}
