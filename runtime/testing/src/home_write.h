#ifndef RUNTIME_TESTING_HOME_WRITE_H
#define RUNTIME_TESTING_HOME_WRITE_H

#include "home_prepare.h"
#include "spisor/status.h"

/**
 * Writes the prepared home through runtime's own writers, every file below its base.
 *
 * @return OK; REFUSED when a file cannot be written; OUT_OF_MEMORY. Whatever was written stays: the caller empties the
 * base when this does not answer OK.
 */
spisor_status runtime_testing_write_home(const runtime_testing_prepared_home *home, spisor_refusal *refusal);

/**
 * Places one prepared install below layout: its bundle written and marked complete as cache.install marks it, and each
 * of its settings set through config's set, so a secret one lands in the secret store and not in a settings file.
 *
 * @return OK; REFUSED when a file cannot be written; what config's set answers; OUT_OF_MEMORY.
 */
spisor_status runtime_testing_place_install(const runtime_testing_layout *layout,
                                            const runtime_testing_prepared_install *install, spisor_refusal *refusal);

/** Removes everything inside base, leaving the folder itself. OK; REFUSED; OUT_OF_MEMORY. */
spisor_status runtime_testing_empty_base(const char *base);

#endif
