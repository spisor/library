#include <stdlib.h>
#include <string.h>

#include "allocation.h"
#include "check.h"
#include "spisor/status.h"

static void numbers_are_the_standards(void) {
    static const spisor_status in_order[] = {SPISOR_OK, SPISOR_NONE, SPISOR_MALFORMED, SPISOR_REFUSED,
                                             SPISOR_OUT_OF_MEMORY};
    for (size_t number = 0; number < sizeof in_order / sizeof in_order[0]; number++) {
        CHECK((size_t)in_order[number] == number);
    }
}

static void a_refusal_holds_copies_until_released(void) {
    char detail[] = "the settings file cannot be read";
    char fix[] = "check the file's permissions";
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(spisor_refuse(&refusal, detail, fix) == SPISOR_REFUSED);
    CHECK(refusal.storage != NULL);
    CHECK(refusal.detail != detail && refusal.fix != fix);
    detail[0] = 'X';
    fix[0] = 'X';
    check_text(refusal.detail, "the settings file cannot be read", "detail");
    check_text(refusal.fix, "check the file's permissions", "fix");
    spisor_refusal_release(&refusal);
    CHECK(refusal.size == sizeof refusal);
    CHECK(refusal.detail == NULL && refusal.fix == NULL && refusal.storage == NULL);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void a_missing_fix_stays_missing(void) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    CHECK(spisor_refuse(&refusal, "no such key", NULL) == SPISOR_REFUSED);
    check_text(refusal.detail, "no such key", "detail");
    CHECK(refusal.fix == NULL);
    spisor_refusal_release(&refusal);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void no_refusal_wanted_allocates_nothing(void) {
    spisor_memory_fail_at(0);
    CHECK(spisor_refuse(NULL, "detail", "fix") == SPISOR_REFUSED);
    CHECK(spisor_memory_attempts() == 0);
}

static void a_refusal_that_is_not_empty_is_left_as_it_was(void) {
    spisor_refusal filled = SPISOR_INIT(spisor_refusal);
    spisor_refusal short_one = {(uint32_t)(sizeof short_one - 1), NULL, NULL, NULL};
    const char *first_detail;
    CHECK(spisor_refuse(&filled, "first", NULL) == SPISOR_REFUSED);
    first_detail = filled.detail;
    spisor_memory_fail_at(0);
    CHECK(spisor_refuse(&filled, "second", "fix") == SPISOR_REFUSED);
    CHECK(filled.detail == first_detail && filled.fix == NULL);
    CHECK(spisor_refuse(&short_one, "detail", "fix") == SPISOR_REFUSED);
    CHECK(short_one.detail == NULL && short_one.fix == NULL && short_one.storage == NULL);
    CHECK(spisor_memory_attempts() == 0);
    spisor_refusal_release(&filled);
    spisor_refusal_release(&short_one);
    spisor_refusal_release(NULL);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void a_failed_allocation_leaves_the_refusal_empty(void) {
    spisor_refusal refusal = SPISOR_INIT(spisor_refusal);
    spisor_memory_fail_at(1);
    CHECK(spisor_refuse(&refusal, "detail", "fix") == SPISOR_OUT_OF_MEMORY);
    CHECK(spisor_memory_failed());
    CHECK(spisor_memory_attempts() == 1);
    spisor_memory_fail_at(0);
    CHECK(refusal.size == sizeof refusal);
    CHECK(refusal.detail == NULL && refusal.fix == NULL && refusal.storage == NULL);
    CHECK(spisor_memory_live_blocks() == 0);
}

static void a_planted_heap_overflow(void) {
    volatile size_t past = 4;
    char *block = malloc(4);
    CHECK(block != NULL);
    if (block != NULL) {
        block[past] = 'x';
        free(block);
    }
}

int main(void) {
    a_planted_heap_overflow();
    numbers_are_the_standards();
    a_refusal_holds_copies_until_released();
    a_missing_fix_stays_missing();
    no_refusal_wanted_allocates_nothing();
    a_refusal_that_is_not_empty_is_left_as_it_was();
    a_failed_allocation_leaves_the_refusal_empty();
    return check_summary("status_test");
}
