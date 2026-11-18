#include "catalog.h"
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_answer.h"
#include "catalog_read.h"
#include "json_service.h"

/** The refusal of an index the schema or the rules refuse, naming the JSON pointer at fault; NULL out of memory. */
static cJSON *refuse_index(const runtime_catalog_refusal *refusal) {
    char *message = runtime_catalog_refusal_message(refusal);
    cJSON *refused = message == NULL ? NULL : runtime_refusal(message, refusal->fix);
    spisor_memory_release(message);
    return refused;
}

cJSON *runtime_catalog_read(const char *text, const char *origin, cJSON **refusal) {
    *refusal = NULL;
    if (text == NULL) {
        return runtime_catalog_refuse_member("text", "the catalog index's text", refusal);
    }
    if (origin == NULL || origin[0] == 0) {
        return runtime_catalog_refuse_member("origin", "the url the index was read from", refusal);
    }
    runtime_catalog_refusal index_refusal;
    cJSON *catalog = NULL;
    spisor_status status = runtime_catalog_read_text(text, strlen(text), origin, &catalog, &index_refusal);
    if (status != SPISOR_OK) {
        *refusal = status == SPISOR_REFUSED ? refuse_index(&index_refusal) : NULL;
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
