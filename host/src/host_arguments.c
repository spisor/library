#include "host_arguments.h"
#include "allocation.h"

#include <stdlib.h>
#include <windows.h>

static char *utf8_of(const wchar_t *wide) {
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    char *narrow = bytes <= 0 ? NULL : spisor_memory_allocate((size_t)bytes);
    if (narrow != NULL && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, narrow, bytes, NULL, NULL) != bytes) {
        spisor_memory_release(narrow);
        narrow = NULL;
    }
    return narrow;
}

char **host_utf8_arguments(int count, wchar_t **wide) {
    char **arguments = spisor_memory_allocate_zeroed((size_t)count + 1, sizeof *arguments);
    for (int index = 0; arguments != NULL && index < count; index++) {
        arguments[index] = utf8_of(wide[index]);
        if (arguments[index] == NULL) {
            host_free_arguments(arguments, index);
            return NULL;
        }
    }
    return arguments;
}

void host_free_arguments(char **arguments, int count) {
    for (int index = 0; arguments != NULL && index < count; index++) {
        spisor_memory_release(arguments[index]);
    }
    spisor_memory_release(arguments);
}
