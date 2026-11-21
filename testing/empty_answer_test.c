#include <stdio.h>

#include "described_checks.h"

/**
 * @file
 * Calls every operation whose answer holds arrays, through the built shared library as a consumer would and from the
 * description each definition answers, with an empty argument and an answer whose storage is NULL but one count is
 * not zero. Each must refuse it MALFORMED and leave the answer as it was: a fill appends at its count, so going on
 * would write past the array.
 */

int main(void) {
    size_t failures = spisor_check_library(SPISOR_DESCRIBED_LIBRARY_PATH, SPISOR_DESCRIBED_DECLARATION_PATH,
                                           spisor_check_counted_answers);
    if (failures == 0) {
        printf("empty_answer_test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "empty_answer_test: %zu checks failed\n", failures);
    return 1;
}
