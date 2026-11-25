#ifndef RUNTIME_FETCH_FILE_H
#define RUNTIME_FETCH_FILE_H

#include "naula/transport.h"

/** Copies the file a file: url names into the sink. */
void runtime_fetch_file_url(const char *url, naula_fetch_sink *sink, naula_fetch_answer *answer);

/**
 * Makes every file: fetch whose url contains containing wait milliseconds first; NULL stops it.
 *
 * @implNote Defined only when SPISOR_FETCH_TEST_HOOKS is set, which only the test's own build of the fetch sources
 * does.
 */
void runtime_fetch_stall_file_urls(const char *containing, unsigned milliseconds);

#endif
