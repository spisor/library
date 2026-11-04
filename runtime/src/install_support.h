#ifndef RUNTIME_INSTALL_SUPPORT_H
#define RUNTIME_INSTALL_SUPPORT_H

#include "cJSON.h"

/** argument.places.<name> when it is an absolute path, else NULL. */
const char *runtime_install_place(const cJSON *argument, const char *name);

/** The string member name of object, or NULL when absent or no string. */
const char *runtime_install_text(const cJSON *object, const char *name);

/** Adds a copy of value to object under name; 1 when value is NULL or the copy was added. */
int runtime_install_copy_into(cJSON *object, const char *name, const cJSON *value);

/** A new argument {places} carrying a copy of places; NULL when memory runs out. */
cJSON *runtime_install_places_call(const cJSON *places);

/** Adds the string text to array; 0 when memory runs out. */
int runtime_install_add_text(cJSON *array, const char *text);

/** Whether array, an array of strings, holds text. */
int runtime_install_holds(const cJSON *array, const char *text);

#endif
