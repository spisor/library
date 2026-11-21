#include <stdio.h>

#include "described_checks.h"

/**
 * @file
 * Calls every operation of every definition the library declares, through the built shared library as a consumer
 * would and from the description each answers, with each argument and answer short of its first shape: each must
 * refuse it MALFORMED and leave every answer as it was, since reading or writing past a consumer's `size` reaches
 * memory the consumer does not own.
 */

int main(void) {
    size_t failures = spisor_check_library(SPISOR_DESCRIBED_LIBRARY_PATH, SPISOR_DESCRIBED_DECLARATION_PATH,
                                           spisor_check_short_shapes);
    if (failures == 0) {
        printf("short_shape_test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "short_shape_test: %zu checks failed\n", failures);
    return 1;
}
