#include "spisor/json_allocation.h"

#include "allocation.h"
#include "cJSON.h"

void spisor_json_use_library_allocator(void) {
    cJSON_Hooks hooks = {spisor_memory_allocate, spisor_memory_release};
    cJSON_InitHooks(&hooks);
}
