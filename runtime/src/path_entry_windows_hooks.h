#ifndef RUNTIME_PATH_ENTRY_WINDOWS_HOOKS_H
#define RUNTIME_PATH_ENTRY_WINDOWS_HOOKS_H

/**
 * The value under HKCU\Environment that the test build of path_entry_windows.c reads and writes in
 * place of Path.
 *
 * @implNote Only a build with SPISOR_PATH_ENTRY_TEST_HOOKS uses it. Every test executable links that
 * build and the library never does, so no test can write the user's real Path.
 */
#define RUNTIME_PATH_ENTRY_TEST_VALUE L"SPISOR_TEST_PATH"

#endif
