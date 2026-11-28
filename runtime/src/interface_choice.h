#ifndef RUNTIME_INTERFACE_CHOICE_H
#define RUNTIME_INTERFACE_CHOICE_H

#include "cJSON.h"

/** spisor-host's setting naming the interface item an install installs, or RUNTIME_HEADLESS for none. */
#define RUNTIME_INTERFACE_SETTING "interface"
#define RUNTIME_HEADLESS "none"

typedef enum runtime_interface_choice_kind {
    RUNTIME_INTERFACE_CHOSEN = 1,
    RUNTIME_INTERFACE_HEADLESS = 2,
    RUNTIME_INTERFACE_UNCHOSEN = 3,
    RUNTIME_INTERFACE_OUT_OF_MEMORY = 4
} runtime_interface_choice_kind;

/**
 * Which interface an install has: CHOSEN with the `id` the setting names, else the catalog's one interface item;
 * HEADLESS when the setting says none or the catalog lists no interface item; UNCHOSEN when it lists two or more and
 * the setting names none. `listed` holds the ids of every interface item the catalog lists, in its order, for the
 * messages that name them; free it with runtime_free_interface_choice.
 */
typedef struct runtime_interface_choice {
    runtime_interface_choice_kind kind;
    const char *id;
    cJSON *listed;
} runtime_interface_choice;

/** The choice spisor-host's settings `values` and `catalog`, {items}, make; `id` points into either. */
runtime_interface_choice runtime_choose_interface(const cJSON *values, const cJSON *catalog);

void runtime_free_interface_choice(runtime_interface_choice *choice);

/** The ids joined by commas, freed; NULL when memory runs out. */
char *runtime_joined_interface_ids(const cJSON *ids);

#endif
