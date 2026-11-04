#include "catalog.h"

#include <stdlib.h>
#include <string.h>

#include "catalog_answer.h"
#include "catalog_read.h"
#include "spisor/json_kind.h"

static int32_t refuse_index(const runtime_catalog_refusal *refusal, uint8_t **out, size_t *out_len) {
    char *message = runtime_catalog_refusal_message(refusal);
    int32_t status = message == NULL ? runtime_catalog_answer(NULL, out, out_len)
                                     : spisor_fail(message, refusal->fix, out, out_len);
    free(message);
    return status;
}

static int32_t read_catalog(const cJSON *argument, uint8_t **out, size_t *out_len) {
    const char *text = runtime_catalog_text_member(argument, "text");
    const char *origin = runtime_catalog_text_member(argument, "origin");
    if (text == NULL) {
        return runtime_catalog_refuse_argument("text", "the catalog index's text", out, out_len);
    }
    if (origin == NULL || origin[0] == 0) {
        return runtime_catalog_refuse_argument("origin", "the url the index was read from", out, out_len);
    }
    runtime_catalog_refusal refusal;
    cJSON *catalog = runtime_catalog_read_text(text, strlen(text), origin, &refusal);
    if (catalog == NULL) {
        int32_t status = refuse_index(&refusal, out, out_len);
        runtime_catalog_free_refusal(&refusal);
        return status;
    }
    cJSON *result = cJSON_CreateObject();
    if (!runtime_catalog_attach(result, "catalog", catalog)) {
        cJSON_Delete(result);
        return runtime_catalog_answer(NULL, out, out_len);
    }
    return runtime_catalog_answer(result, out, out_len);
}

RUNTIME_EXPORT int32_t runtime_catalog_read(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    spisor_call call;
    int32_t status = spisor_parse_call(in, in_len, 0, &call, out, out_len);
    if (status != SPISOR_STATUS_SUCCESS) {
        return status;
    }
    status = read_catalog(call.argument, out, out_len);
    spisor_discard_call(&call);
    return status;
}
