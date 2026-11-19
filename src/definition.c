#include "spisor/definition.h"

#include <stdio.h>

int spisor_has_shape(uint32_t size, size_t shape) {
    return size >= shape;
}

spisor_status spisor_refuse_as(spisor_refusal *refusal, spisor_status status, const char *detail, const char *fix) {
    return spisor_refuse(refusal, detail, fix) == SPISOR_OUT_OF_MEMORY ? SPISOR_OUT_OF_MEMORY : status;
}

spisor_status spisor_refuse_shape(spisor_refusal *refusal, const char *operation) {
    char detail[160];
    snprintf(detail, sizeof detail, "the argument or the answer of %s is absent or short of its first shape, or the "
             "answer is not empty", operation);
    return spisor_refuse_as(refusal, SPISOR_MALFORMED, detail,
                            "pass the argument and an empty answer, each with its size set as SPISOR_INIT sets it");
}

void spisor_release_refusal(void *context, spisor_refusal *refusal) {
    (void)context;
    spisor_refusal_release(refusal);
}

void spisor_overwrite(void *bytes, size_t length) {
    volatile unsigned char *cursor = bytes;
    while (length-- > 0) {
        *cursor++ = 0;
    }
}
