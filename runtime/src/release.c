#include <stdint.h>

#include "export.h"
#include "spisor/json_kind.h"

RUNTIME_EXPORT void runtime_release(uint8_t *buffer) {
    spisor_release(buffer);
}
