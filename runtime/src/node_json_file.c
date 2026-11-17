#include "node_json_file.h"

#include <stdint.h>
#include <stdlib.h>

#include "spisor/file.h"
#include "spisor/javascript.h"
#include "json_parse.h"
#include "spisor/json_kind.h"

cJSON *runtime_read_node_json_noting(const char *path, int *out_of_memory) {
    size_t length = 0;
    uint8_t *bytes = spisor_read_file(path, &length);
    *out_of_memory = 0;
    if (bytes == NULL) {
        return NULL;
    }
    size_t decoded_length = 0;
    uint8_t *decoded = spisor_decode_utf8_lossy(bytes, length, &decoded_length);
    free(bytes);
    *out_of_memory = decoded == NULL;
    cJSON *tree = decoded == NULL ? NULL : spisor_parse_json_reporting(decoded, decoded_length, out_of_memory);
    free(decoded);
    return tree;
}

cJSON *runtime_read_node_json(const char *path) {
    int out_of_memory;
    return runtime_read_node_json_noting(path, &out_of_memory);
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
