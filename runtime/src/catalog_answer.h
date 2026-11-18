#ifndef RUNTIME_CATALOG_ANSWER_H
#define RUNTIME_CATALOG_ANSWER_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** Sets *refusal to the refusal of an argument that carries no usable member and answers NULL; *refusal is NULL out of memory. */
cJSON *runtime_catalog_refuse_member(const char *member, const char *shape, cJSON **refusal);

/** Adds item to object under name; on failure item is freed. 0 when item is NULL or the add fails. */
int runtime_catalog_attach(cJSON *object, const char *name, cJSON *item);

/** The string member of argument, or NULL when absent or not a string. */
const char *runtime_catalog_text_member(const cJSON *argument, const char *name);

#endif
