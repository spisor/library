#include "spisor/json_allocation.h"

/**
 * @file
 * Installs cJSON's hooks once when the runtime library loads, before any thread of the host can reach its cJSON, since
 * cJSON_InitHooks first resets them to malloc and free: installing them later could let one allocation and its free
 * use different allocators. A binary that links runtime's objects statically installs them on the first line of its
 * main instead.
 */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        spisor_json_use_library_allocator();
    }
    return TRUE;
}

#else

__attribute__((constructor, used)) static void install_json_allocator(void) {
    spisor_json_use_library_allocator();
}

#endif
