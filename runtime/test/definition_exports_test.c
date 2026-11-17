#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "check.h"
#include "definition_symbols.h"

/**
 * @file
 * Loads the built shared runtime as a consumer would and requires every `runtime_*_definition_of` that
 * a header under `include/runtime` declares to be among its exports. The names come from the headers at configure time, so a
 * service added later is checked without this file changing.
 */

static const char *const declared_symbols[] = {RUNTIME_DEFINITION_SYMBOLS};

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_runtime(void) {
    return LoadLibraryA(RUNTIME_LIBRARY_PATH);
}

static int exports(library_handle library, const char *symbol) {
    return GetProcAddress(library, symbol) != NULL;
}

static void unload_runtime(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_runtime(void) {
    return dlopen(RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
}

static int exports(library_handle library, const char *symbol) {
    return dlsym(library, symbol) != NULL;
}

static void unload_runtime(library_handle library) {
    dlclose(library);
}
#endif

static void every_declared_definition_is_exported(void) {
    library_handle library = load_runtime();
    CHECK(library != NULL);
    if (library == NULL) {
        fprintf(stderr, "could not load %s\n", RUNTIME_LIBRARY_PATH);
        return;
    }
    size_t count = sizeof declared_symbols / sizeof declared_symbols[0];
    CHECK(count > 0);
    for (size_t index = 0; index < count; index++) {
        if (!exports(library, declared_symbols[index])) {
            fprintf(stderr, "%s is declared but not exported\n", declared_symbols[index]);
            check_failures++;
        }
    }
    unload_runtime(library);
}

int main(void) {
    every_declared_definition_is_exported();
    return check_summary("definition_exports_test");
}
