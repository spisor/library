#ifndef RUNTIME_TESTING_HOME_LAYOUT_H
#define RUNTIME_TESTING_HOME_LAYOUT_H

#include <stddef.h>

#include "cJSON.h"
#include "runtime/environment.h"

/** How many variables point a process at a laid-out home. */
#define RUNTIME_TESTING_VARIABLE_COUNT 6

/**
 * Where each place of a laid-out home lies below its base, every path absolute, with the secrets file the places
 * resolve to once the home's variables are set.
 */
typedef struct runtime_testing_layout {
    char *user;
    char *portable;
    char *state;
    char *cache;
    char *secrets;
    char *application;
    char *catalog;
    char *secrets_file;
    int force_file_store;
} runtime_testing_layout;

/** Fills layout with the places below base. 1, or 0 with layout empty when memory runs out. */
int runtime_testing_layout_of(const char *base, runtime_testing_layout *layout);

void runtime_testing_free_layout(runtime_testing_layout *layout);

/** The variables that point a process at layout, borrowing its paths. */
void runtime_testing_layout_variables(const runtime_testing_layout *layout,
                                      runtime_variable variables[RUNTIME_TESTING_VARIABLE_COUNT]);

/** The variables of layout as the JSON environment runtime's C callers take; NULL when memory runs out. */
cJSON *runtime_testing_layout_environment(const runtime_testing_layout *layout);

#endif
