#ifndef RUNTIME_TESTING_HOME_LAYOUT_H
#define RUNTIME_TESTING_HOME_LAYOUT_H

/** Where each place of a laid-out home lies below its base, every path absolute. */
typedef struct runtime_testing_layout {
    char *user;
    char *portable;
    char *state;
    char *cache;
    char *secrets;
    char *application;
    char *catalog;
    char *koffi;
} runtime_testing_layout;

/** Fills layout with the places below base. 1, or 0 with layout empty when memory runs out. */
int runtime_testing_layout_of(const char *base, runtime_testing_layout *layout);

void runtime_testing_free_layout(runtime_testing_layout *layout);

#endif
