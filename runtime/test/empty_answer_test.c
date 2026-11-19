#include <stdio.h>

#include "definition_calls.h"

/**
 * @file
 * Calls every operation whose answer holds arrays, through the built shared runtime as a consumer would, with an
 * empty argument and an answer whose storage is NULL but one count is not zero. Each must refuse it MALFORMED and leave
 * the answer as it was: a fill appends at its count, so going on would write past the array.
 */

int main(void) {
    int failures = spisor_check_counted_answers(SPISOR_DEFINITION_LIBRARY_PATH);
    if (failures == 0) {
        printf("empty_answer_test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "empty_answer_test: %d checks failed\n", failures);
    return 1;
}
