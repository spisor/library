#include <stdio.h>

#include "definition_calls.h"

/**
 * @file
 * Calls every operation of every definition the runtime headers declare, through the built shared runtime as a
 * consumer would, with each argument and answer short of its first shape: each must refuse it MALFORMED and leave
 * every answer as it was, since reading or writing past a consumer's `size` reaches memory the consumer does not own.
 */

int main(void) {
    int failures = spisor_check_short_shapes(SPISOR_DEFINITION_LIBRARY_PATH);
    if (failures == 0) {
        printf("short_shape_test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "short_shape_test: %d checks failed\n", failures);
    return 1;
}
