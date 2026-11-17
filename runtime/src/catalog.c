#include "catalog.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_answer.h"
#include "catalog_read.h"
#include "json_service.h"

/**
 * The refusal of an index the schema or the rules refuse, naming the JSON pointer at fault; NULL out of memory, which
 * the reader reports as a refusal with the out-of-memory fix.
 */
static cJSON *refuse_index(const runtime_catalog_refusal *refusal) {
    if (refusal->fix == NULL || strcmp(refusal->fix, RUNTIME_CATALOG_OUT_OF_MEMORY_FIX) == 0) {
        return NULL;
    }
    char *message = runtime_catalog_refusal_message(refusal);
    cJSON *refused = message == NULL ? NULL : runtime_refusal(message, refusal->fix);
    free(message);
    return refused;
}

cJSON *runtime_catalog_read_operation(const cJSON *argument, cJSON **refusal) {
    const char *text = runtime_catalog_text_member(argument, "text");
    const char *origin = runtime_catalog_text_member(argument, "origin");
    if (text == NULL) {
        return runtime_catalog_refuse_member("text", "the catalog index's text", refusal);
    }
    if (origin == NULL || origin[0] == 0) {
        return runtime_catalog_refuse_member("origin", "the url the index was read from", refusal);
    }
    runtime_catalog_refusal index_refusal;
    cJSON *catalog = runtime_catalog_read_text(text, strlen(text), origin, &index_refusal);
    if (catalog == NULL) {
        *refusal = refuse_index(&index_refusal);
        runtime_catalog_free_refusal(&index_refusal);
        return NULL;
    }
    cJSON *result = cJSON_CreateObject();
    if (!runtime_catalog_attach(result, "catalog", catalog)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}
