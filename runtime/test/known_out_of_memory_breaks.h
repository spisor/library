#ifndef KNOWN_OUT_OF_MEMORY_BREAKS_H
#define KNOWN_OUT_OF_MEMORY_BREAKS_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/**
 * A case whose failing allocations break its contract until the finding it names is fixed. Each entry goes in the
 * change that fixes it: a listed case that holds fails the suite.
 */
typedef struct known_out_of_memory_break {
    const char *suite;
    const char *label;
    int sticky;
    const char *finding;
} known_out_of_memory_break;

static const known_out_of_memory_break known_out_of_memory_breaks[] = {
    {"", "", 0, ""},
};

static const known_out_of_memory_break *known_out_of_memory_break_of(const char *suite, const char *label,
                                                                     int sticky) {
    size_t count = sizeof known_out_of_memory_breaks / sizeof known_out_of_memory_breaks[0];
    for (size_t index = 0; index < count; index++) {
        const known_out_of_memory_break *known = &known_out_of_memory_breaks[index];
        if (strcmp(known->suite, suite) == 0 && strcmp(known->label, label) == 0 && known->sticky == sticky) {
            return known;
        }
    }
    return NULL;
}

/** The check failures a finished case adds: its own when unlisted, none when a listed case broke, one when it held. */
static int settled_out_of_memory_failures(const char *suite, const char *label, int sticky, int failures) {
    const known_out_of_memory_break *known = known_out_of_memory_break_of(suite, label, sticky);
    if (known == NULL) {
        return failures;
    }
    if (failures > 0) {
        printf("%s%s: known out-of-memory break, %s\n", label, sticky ? " (sticky)" : "", known->finding);
        return 0;
    }
    fprintf(stderr, "%s%s: holds now, so its entry in known_out_of_memory_breaks goes\n", label,
            sticky ? " (sticky)" : "");
    return 1;
}

#endif
