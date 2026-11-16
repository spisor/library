#include "check.h"
#include "vector_reader.h"

int main(void) {
    cJSON *vector = spisor_vector_read(SPISOR_VECTOR_READER_VECTOR_PATH);
    spisor_value_tree tree = SPISOR_INIT(spisor_value_tree);
    CHECK(vector != NULL);
    if (vector != NULL) {
        CHECK(spisor_vector_value(vector, "value", &tree));
        CHECK(tree.root.kind == SPISOR_VALUE_RECORD && tree.root.count == 4);
        spisor_value_release(&tree);
        cJSON_Delete(vector);
    }
    return check_summary("vector_reader_shipped_allocator_test");
}
