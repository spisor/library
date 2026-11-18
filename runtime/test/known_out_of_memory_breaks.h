#ifndef KNOWN_OUT_OF_MEMORY_BREAKS_H
#define KNOWN_OUT_OF_MEMORY_BREAKS_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define BREAKS_ON_WINDOWS 1u
#define BREAKS_ON_POSIX 2u
#define BREAKS_ON_BOTH (BREAKS_ON_WINDOWS | BREAKS_ON_POSIX)
#if defined(_WIN32)
#define BREAKS_HERE BREAKS_ON_WINDOWS
#else
#define BREAKS_HERE BREAKS_ON_POSIX
#endif

/**
 * A case whose failing allocations break its contract, on the platforms named, until the finding it names is fixed.
 * Each entry goes in the change that fixes it: a listed case that holds where it is listed fails the suite, and on a
 * platform it is not listed for the case must hold.
 *
 * @implNote A wide path conversion, for one, allocates on Windows alone, so a case can break there and hold on POSIX.
 */
typedef struct known_out_of_memory_break {
    const char *suite;
    const char *label;
    int sticky;
    unsigned breaks_on;
    const char *finding;
} known_out_of_memory_break;

#define OUT_OF_MEMORY_SUITE "out_of_memory_test"
#define OPERATIONS_SUITE "out_of_memory_install_operations_test"
#define ECOSYSTEM_SUITE "out_of_memory_ecosystem_test"

static const known_out_of_memory_break known_out_of_memory_breaks[] = {
    {OPERATIONS_SUITE, "hosts' register_host", 0, BREAKS_ON_POSIX,
     "measured in CI only, not yet traced: failing allocation 825 answers OK with another tree; the POSIX Path "
     "change is a profile line, and change_kinds.c runtime_change_is_on_disk reads a failed profile read as "
     "the line being absent"},
};

static const known_out_of_memory_break *known_out_of_memory_break_of(const char *suite, const char *label,
                                                                     int sticky) {
    size_t count = sizeof known_out_of_memory_breaks / sizeof known_out_of_memory_breaks[0];
    for (size_t index = 0; index < count; index++) {
        const known_out_of_memory_break *known = &known_out_of_memory_breaks[index];
        if (strcmp(known->suite, suite) == 0 && strcmp(known->label, label) == 0 && known->sticky == sticky &&
            (known->breaks_on & BREAKS_HERE) != 0) {
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
