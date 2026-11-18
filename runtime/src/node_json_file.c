#include "node_json_file.h"

#include <stdint.h>
#include <stdlib.h>

#include "allocation.h"
#include "file_reading.h"
#include "json_parse.h"
#include "spisor/javascript.h"
#include "spisor/json_kind.h"

cJSON *runtime_parse_node_json(const uint8_t *bytes, size_t length, int *out_of_memory) {
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, length, &decoded_length);
    *out_of_memory = decoded == NULL;
    cJSON *tree = decoded == NULL ? NULL : spisor_parse_json_reporting(decoded, decoded_length, out_of_memory);
    spisor_memory_release(decoded);
    return tree;
}

cJSON *runtime_read_node_json(const char *path, int *out_of_memory) {
    size_t length = 0;
    spisor_read_outcome outcome;
    uint8_t *bytes = spisor_read_file_reporting(path, &length, &outcome);
    *out_of_memory = outcome == SPISOR_READ_OUT_OF_MEMORY;
    if (bytes == NULL) {
        return NULL;
    }
    cJSON *tree = runtime_parse_node_json(bytes, length, out_of_memory);
    spisor_memory_release(bytes);
    return tree;
}

int runtime_is_version_map(const cJSON *value) {
    if (!cJSON_IsObject(value)) {
        return 0;
    }
    for (const cJSON *member = value->child; member != NULL; member = member->next) {
        if (!cJSON_IsString(member)) {
            return 0;
        }
    }
    return 1;
}
