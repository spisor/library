#ifndef RUNTIME_SECRET_WINDOWS_FAULT_H
#define RUNTIME_SECRET_WINDOWS_FAULT_H

/**
 * Makes every chunk write after the next `count` fail, so a test can interrupt a chunked set midway;
 * a negative count (the initial state) never fails.
 *
 * @implNote Defined only when SPISOR_SECRET_TEST_HOOKS is set, which only the test's own build of
 * secret_windows.c does; the library carries no such symbol. Only secret_store_test calls this. The real store offers no other way to make one
 * credential write fail while its neighbours succeed.
 */
void runtime_secret_windows_allow_chunk_writes(long count);

/** Likewise for the read of the base entry that decides which chunk generation is live. */
void runtime_secret_windows_allow_base_reads(long count);

#endif
