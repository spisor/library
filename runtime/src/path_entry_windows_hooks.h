#ifndef RUNTIME_PATH_ENTRY_WINDOWS_HOOKS_H
#define RUNTIME_PATH_ENTRY_WINDOWS_HOOKS_H

#include <wchar.h>

/**
 * The name of the value under HKCU\Environment that the test build of path_entry_windows.c reads and writes in place
 * of Path: SPISOR_TEST_PATH_ and the id of the test process, the same in every process that test starts.
 *
 * @implNote Only a build with SPISOR_PATH_ENTRY_TEST_HOOKS uses it. Every test executable links that build and the
 * library never does, so no test can write the user's real Path.
 */
const wchar_t *runtime_path_entry_test_value(void);

#endif
