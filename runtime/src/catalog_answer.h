#ifndef RUNTIME_CATALOG_ANSWER_H
#define RUNTIME_CATALOG_ANSWER_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** Answers result, which it frees; a NULL result answers out of memory. */
int32_t runtime_catalog_answer(cJSON *result, uint8_t **out, size_t *out_len);

/** Refuses an argument that carries no usable member, saying what shape it takes. */
int32_t runtime_catalog_refuse_argument(const char *member, const char *shape, uint8_t **out, size_t *out_len);

/** Adds item to object under name; on failure item is freed. 0 when item is NULL or the add fails. */
int runtime_catalog_attach(cJSON *object, const char *name, cJSON *item);

/** The string member of argument, or NULL when absent or not a string. */
const char *runtime_catalog_text_member(const cJSON *argument, const char *name);

#endif
