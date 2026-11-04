#ifndef RUNTIME_CONFIG_HOOKS_H
#define RUNTIME_CONFIG_HOOKS_H

/**
 * Makes the first settings write after this call pause once it has read the layers: it creates
 * signals/paused, then waits until signals/release exists (ten seconds at most), so a test can start a
 * second writer while the first holds what it read; NULL stops it.
 *
 * @implNote Defined only when SPISOR_CONFIG_TEST_HOOKS is set, which only the test's own build of the
 * runtime objects does; the library carries no such symbol.
 */
void runtime_config_pause_after_read(const char *signals);

#endif
