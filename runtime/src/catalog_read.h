#ifndef RUNTIME_CATALOG_READ_H
#define RUNTIME_CATALOG_READ_H

#include <stddef.h>

#include "cJSON.h"

/** Why an index is refused: what is wrong, how to correct it, and the JSON pointer it names, or NULL. */
typedef struct runtime_catalog_refusal {
    char *detail;
    char *fix;
    char *path;
} runtime_catalog_refusal;

/**
 * Takes over detail and path, which may be NULL; fix is copied. Answers 0, so a check can end with it.
 * Out of memory the detail says so.
 */
int runtime_catalog_refuse(runtime_catalog_refusal *refusal, char *detail, const char *fix, char *path);

void runtime_catalog_free_refusal(runtime_catalog_refusal *refusal);

/** The detail followed by " at " and the path when it names one; freed with free, NULL out of memory. */
char *runtime_catalog_refusal_message(const runtime_catalog_refusal *refusal);

/**
 * The catalog index in text, checked against catalog.schema.json and the rules no schema can say, with
 * every file url resolved against origin, the index's own url; or NULL and the refusal.
 *
 * @implNote A leading byte order mark is dropped, as an editor on Windows writes one. A plugin item
 * without runtime answers runtime javascript, so no consumer restates that default.
 */
cJSON *runtime_catalog_read_text(const char *text, size_t length, const char *origin,
                                 runtime_catalog_refusal *refusal);

#endif
