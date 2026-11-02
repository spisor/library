#ifndef RUNTIME_SECRET_LINUX_HOOKS_H
#define RUNTIME_SECRET_LINUX_HOOKS_H

/**
 * Forgets any loaded Secret Service library and makes the next call load the shared object called
 * `library_name` instead of "libsecret-1.so.0"; NULL restores the default.
 *
 * @implNote Defined only when SPISOR_SECRET_TEST_HOOKS is set, which only the test's own build of
 * secret_linux.c does; the library carries no such symbol. The runner's packages decide whether the real
 * library exists, so a test that needs it absent names one that cannot exist.
 */
void runtime_secret_linux_use_library(const char *library_name);

/** Whether the library named above opened and every symbol this backend needs resolved. */
int runtime_secret_linux_library_is_loaded(void);

#endif
