#include "standard_refusal.h"

#include <string.h>

#include "spisor/definition.h"

#define REFUSAL_SHAPE SPISOR_SHAPE_THROUGH(weivel_standard_service_refusal, storage)

int runtime_is_usable_service_refusal(const weivel_standard_service_refusal *refusal) {
    return refusal == NULL || (spisor_has_shape(refusal->size, REFUSAL_SHAPE) && refusal->storage == NULL);
}

void runtime_zero_beyond(void *answer, size_t provided) {
    uint32_t size;
    memcpy(&size, answer, sizeof size);
    if (size > provided) {
        memset((char *)answer + provided, 0, size - provided);
    }
}

weivel_status runtime_answer_service(spisor_status status, spisor_refusal *filled,
                                     weivel_standard_service_refusal *refusal) {
    if (refusal == NULL || filled->storage == NULL) {
        spisor_refusal_release(filled);
        return (weivel_status)status;
    }
    refusal->detail = filled->detail;
    refusal->fix = filled->fix;
    refusal->storage = filled->storage;
    runtime_zero_beyond(refusal, sizeof *refusal);
    filled->detail = NULL;
    filled->fix = NULL;
    filled->storage = NULL;
    return (weivel_status)status;
}

void runtime_release_service_refusal(void *context, weivel_standard_service_refusal *refusal) {
    spisor_refusal held = SPISOR_INIT(spisor_refusal);
    (void)context;
    if (refusal == NULL || !spisor_has_shape(refusal->size, REFUSAL_SHAPE) || refusal->storage == NULL) {
        return;
    }
    held.detail = refusal->detail;
    held.fix = refusal->fix;
    held.storage = refusal->storage;
    spisor_refusal_release(&held);
    refusal->detail = NULL;
    refusal->fix = NULL;
    refusal->storage = NULL;
}
