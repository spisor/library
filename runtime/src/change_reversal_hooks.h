#ifndef RUNTIME_CHANGE_REVERSAL_HOOKS_H
#define RUNTIME_CHANGE_REVERSAL_HOOKS_H

/**
 * Lets the next count reversals run and stops the reversal loop before the one after, as a crash
 * there would; a negative count (the initial state) never stops it.
 *
 * @implNote Defined only when SPISOR_CHANGES_TEST_HOOKS is set, which only the test's own build of
 * change_reversal.c does; the library carries no such symbol. Only changes_test calls this, to
 * interrupt a removal halfway without killing the process.
 */
void runtime_changes_allow_reversals(long count);

#endif
