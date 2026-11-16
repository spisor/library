#include "allocation.h"
#include "check.h"
#include "vector_reader.h"

static void the_reader_allocates_through_the_counting_allocator(const cJSON *vector) {
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    spisor_memory_fail_at(1);
    CHECK(!spisor_vector_value(vector, "value", &tree));
    CHECK(spisor_memory_failed() && spisor_memory_attempts() == 1);
    spisor_memory_fail_at(0);
    CHECK(spisor_vector_value(vector, "value", &tree));
    CHECK(spisor_memory_attempts() > 0 && spisor_memory_live_blocks() > 0);
    spisor_value_release(&tree);
    CHECK(spisor_memory_live_blocks() == 0);
}

int main(void) {
    cJSON *vector = spisor_vector_read(SPISOR_VECTOR_READER_VECTOR_PATH);
    CHECK(vector != NULL);
    if (vector != NULL) {
        the_reader_allocates_through_the_counting_allocator(vector);
        cJSON_Delete(vector);
    }
    return check_summary("reader_with_counting_allocator_test");
}
