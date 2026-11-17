#include "answer_storage.h"

#include <stdint.h>
#include <string.h>

#include "allocation.h"
#include "json_value.h"

/**
 * One block's header: the block taken before it, and a tree's storage the block adopted. The union keeps the payload
 * after it aligned for any field an answer holds.
 */
typedef union block_header {
    struct {
        union block_header *previous;
        void *adopted;
    } link;
    double number_alignment;
    long long integer_alignment;
    void *pointer_alignment;
} block_header;

static block_header *chain(void **storage, size_t bytes) {
    block_header *block;
    if (bytes > SIZE_MAX - sizeof(block_header)) {
        return NULL;
    }
    block = spisor_memory_allocate(sizeof(block_header) + bytes);
    if (block == NULL) {
        return NULL;
    }
    memset(block, 0, sizeof(block_header) + bytes);
    block->link.previous = *storage;
    *storage = block;
    return block;
}

void *runtime_storage_take(void **storage, size_t bytes) {
    block_header *block = chain(storage, bytes);
    return block == NULL ? NULL : block + 1;
}

int runtime_storage_take_array(void **storage, size_t count, size_t width, void **array) {
    *array = NULL;
    if (count == 0) {
        return 1;
    }
    if (width != 0 && count > SIZE_MAX / width) {
        return 0;
    }
    *array = runtime_storage_take(storage, count * width);
    return *array != NULL;
}

int runtime_storage_copy(void **storage, const char *text, const char **copy) {
    size_t length;
    char *taken;
    *copy = NULL;
    if (text == NULL) {
        return 1;
    }
    length = strlen(text);
    taken = runtime_storage_take(storage, length + 1);
    if (taken == NULL) {
        return 0;
    }
    memcpy(taken, text, length + 1);
    *copy = taken;
    return 1;
}

int runtime_storage_texts(void **storage, const cJSON *array, const char *const **texts, size_t *count) {
    const cJSON *item;
    const char **copies;
    size_t index = 0;
    *texts = NULL;
    *count = 0;
    cJSON_ArrayForEach(item, array) {
        index += cJSON_IsString(item);
    }
    if (!runtime_storage_take_array(storage, index, sizeof *copies, (void **)&copies)) {
        return 0;
    }
    index = 0;
    cJSON_ArrayForEach(item, array) {
        if (cJSON_IsString(item) && !runtime_storage_copy(storage, item->valuestring, &copies[index++])) {
            return 0;
        }
    }
    *texts = copies;
    *count = index;
    return 1;
}

spisor_status runtime_storage_value(void **storage, const cJSON *json, const spisor_value **value) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    block_header *block;
    spisor_value *root;
    spisor_status status;
    *value = NULL;
    if (json == NULL) {
        return SPISOR_OK;
    }
    block = chain(storage, sizeof *root);
    if (block == NULL) {
        return SPISOR_OUT_OF_MEMORY;
    }
    status = spisor_value_of_json(json, &tree);
    if (status != SPISOR_OK) {
        return status;
    }
    root = (spisor_value *)(block + 1);
    *root = tree.root;
    block->link.adopted = tree.storage;
    *value = root;
    return SPISOR_OK;
}

void runtime_storage_release(void *storage) {
    block_header *block = storage;
    while (block != NULL) {
        block_header *previous = block->link.previous;
        spisor_memory_release(block->link.adopted);
        spisor_memory_release(block);
        block = previous;
    }
}

void runtime_release_stored_answer(void *answer, size_t shape) {
    uint32_t size;
    void *storage;
    if (answer == NULL) {
        return;
    }
    memcpy(&size, answer, sizeof size);
    if (size < shape) {
        return;
    }
    memcpy(&storage, (char *)answer + shape - sizeof storage, sizeof storage);
    runtime_storage_release(storage);
    memset((char *)answer + sizeof size, 0, shape - sizeof size);
}

spisor_status runtime_storage_provenance(void **storage, const cJSON *json, const runtime_registry_provenance **out) {
    runtime_registry_provenance *provenance;
    *out = NULL;
    if (!cJSON_IsObject(json)) {
        return SPISOR_OK;
    }
    provenance = runtime_storage_take(storage, sizeof *provenance);
    if (provenance == NULL || !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "method")), &provenance->method) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "version")), &provenance->version) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "location")), &provenance->location) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "os")), &provenance->os) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "architecture")), &provenance->architecture) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "installedAt")), &provenance->installed_at) ||
        !runtime_storage_copy(storage, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "package")), &provenance->package)) {
        return SPISOR_OUT_OF_MEMORY;
    }
    provenance->size = (uint32_t)sizeof *provenance;
    *out = provenance;
    return runtime_storage_value(storage, cJSON_GetObjectItemCaseSensitive(json, "options"), &provenance->options);
}
