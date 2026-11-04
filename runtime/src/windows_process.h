#ifndef RUNTIME_WINDOWS_PROCESS_H
#define RUNTIME_WINDOWS_PROCESS_H

#include <wchar.h>
#include <windows.h>

#include "cJSON.h"

/** text, UTF-8, as UTF-16, freed with free; NULL when it is not valid UTF-8 or memory runs out. */
wchar_t *runtime_windows_text(const char *text);

/** "executable" and each of arguments, an array of strings or NULL, quoted as CommandLineToArgvW reads them back. */
wchar_t *runtime_windows_command_line(const char *executable, const cJSON *arguments);

/**
 * environment, an object of strings, as CreateProcessW takes it with CREATE_UNICODE_ENVIRONMENT: each
 * NAME=VALUE ended by NUL, sorted by name ignoring case, and one more NUL, so an empty one is two NULs.
 */
wchar_t *runtime_windows_environment_block(const cJSON *environment);

/** The null device, opened for reading and writing as an inheritable handle. */
HANDLE runtime_windows_null_device(void);

/**
 * An attribute list handing a child exactly the count inheritable handles, so it inherits no other handle
 * of the caller's; freed with runtime_windows_free_handle_list. NULL when it cannot be made.
 */
LPPROC_THREAD_ATTRIBUTE_LIST runtime_windows_handle_list(HANDLE *handles, DWORD count);

void runtime_windows_free_handle_list(LPPROC_THREAD_ATTRIBUTE_LIST attributes);

#endif
