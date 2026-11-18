#ifndef RUNTIME_CATALOG_READ_H
#define RUNTIME_CATALOG_READ_H

#include <stddef.h>

#include "cJSON.h"
#include "spisor/status.h"

/** Why an index is refused: what is wrong, how to correct it, and the JSON pointer it names, or NULL. */
typedef struct runtime_catalog_refusal {
    char *detail;
    char *fix;
    char *path;
} runtime_catalog_refusal;

/**
 * Takes over detail and path, which may be NULL; fix is copied. Answers REFUSED with the refusal filled, or
 * OUT_OF_MEMORY with it empty when detail is NULL or fix cannot be copied, so a check can end with it.
 */
spisor_status runtime_catalog_refuse(runtime_catalog_refusal *refusal, char *detail, const char *fix, char *path);

void runtime_catalog_free_refusal(runtime_catalog_refusal *refusal);

/** A REFUSED refusal's detail followed by " at " and the path when it names one; freed, NULL out of memory. */
char *runtime_catalog_refusal_message(const runtime_catalog_refusal *refusal);

/**
 * The catalog index in text, checked against catalog.schema.json and the rules no schema can say, with
 * every file url resolved against origin, the index's own url. OK with *catalog set; REFUSED with the refusal
 * filled; OUT_OF_MEMORY. *catalog is NULL and the refusal empty unless the status says otherwise.
 *
 * @implNote A leading byte order mark is dropped, as an editor on Windows writes one. A plugin item
 * without runtime answers runtime javascript, so no consumer restates that default.
 */
spisor_status runtime_catalog_read_text(const char *text, size_t length, const char *origin, cJSON **catalog,
                                        runtime_catalog_refusal *refusal);

#endif
