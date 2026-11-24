#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <weivel/description.h>
#include <weivel/library.h>

#include "check.h"
#include "declaration_file.h"

/**
 * @file
 * Loads the built shared runtime as a binding would: reads `runtime/library.json` with the standard's reader, and for
 * every service calls the `description` symbol it declares and requires the description to pass the standard's own
 * check, to name `runtime_<id>_definition`, or for a standard service the standard's own struct, and to answer the
 * definition the service's definition function answers, so a description that is declared but not linked, not
 * generated from the service's own struct, or naming another function, fails here.
 */

typedef void (*exported_function)(void);
typedef const weivel_standard_description *(*describe_function)(void);
typedef const void *(*definition_function)(void);

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_runtime(void) {
    return LoadLibraryA(RUNTIME_LIBRARY_PATH);
}

static exported_function exported_of(library_handle library, const char *symbol) {
    FARPROC address = GetProcAddress(library, symbol);
    return address == NULL ? NULL : (exported_function)address;
}

static void unload_runtime(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_runtime(void) {
    return dlopen(RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
}

static exported_function exported_of(library_handle library, const char *symbol) {
    void *address = dlsym(library, symbol);
    exported_function exported = NULL;
    if (address != NULL) {
        memcpy(&exported, &address, sizeof exported);
    }
    return exported;
}

static void unload_runtime(library_handle library) {
    dlclose(library);
}
#endif

static int read_library(weivel_standard_library *library) {
    size_t length = 0;
    char *text = read_declaration_file(&length);
    weivel_status status;
    CHECK(text != NULL);
    if (text == NULL) {
        return 0;
    }
    status = weivel_standard_read_library(text, length, library, NULL);
    free(text);
    CHECK(status == WEIVEL_STATUS_OK);
    return status == WEIVEL_STATUS_OK;
}

/** A standard service's id beside the standard's struct its description must name. */
typedef struct standard_service {
    const char *id;
    const char *definition;
} standard_service;

static const standard_service standard_services[] = {{"weivel.host", "weivel_standard_host"},
                                                     {"weivel.scope", "weivel_standard_scopes"}};

static void check_definition_name(const char *definition, const char *service_id) {
    char expected[128];
    int written = snprintf(expected, sizeof expected, "runtime_%s_definition", service_id);
    CHECK(written > 0 && (size_t)written < sizeof expected);
    for (size_t index = 0; index < sizeof standard_services / sizeof standard_services[0]; index++) {
        if (strcmp(service_id, standard_services[index].id) == 0) {
            snprintf(expected, sizeof expected, "%s", standard_services[index].definition);
        }
    }
    check_text(definition, expected, service_id);
}

static const char *definition_symbol_of(const weivel_standard_library_service *service) {
    for (size_t index = 0; index < service->function_count; index++) {
        if (service->functions[index].kind == WEIVEL_STANDARD_FUNCTION_DEFINITION) {
            return service->functions[index].symbol;
        }
    }
    return NULL;
}

/** Requires `description` to answer the definition the service's own definition function answers. */
static void check_answers(library_handle runtime, const weivel_standard_description *description,
                          const weivel_standard_library_service *service) {
    const char *definition_symbol = definition_symbol_of(service);
    exported_function definer = definition_symbol == NULL ? NULL : exported_of(runtime, definition_symbol);
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    if (definer == NULL) {
        fprintf(stderr, "service %s declares no definition function runtime exports\n", service->id);
        check_failures++;
        return;
    }
    if (weivel_standard_description_answers(description, ((definition_function)definer)(), &refusal) !=
        WEIVEL_STATUS_OK) {
        fprintf(stderr, "service %s: its description does not answer %s, at \"%s\": %s\n", service->id,
                definition_symbol, refusal.path == NULL ? "" : refusal.path,
                refusal.detail == NULL ? "" : refusal.detail);
        weivel_standard_refusal_release(&refusal);
        check_failures++;
    }
}

static void check_description(library_handle runtime, const char *symbol,
                              const weivel_standard_library_service *service) {
    const char *service_id = service->id;
    exported_function describer;
    const weivel_standard_description *description;
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    if (symbol == NULL) {
        fprintf(stderr, "service %s declares no description\n", service_id);
        check_failures++;
        return;
    }
    describer = exported_of(runtime, symbol);
    if (describer == NULL) {
        fprintf(stderr, "service %s declares %s, which runtime does not export\n", service_id, symbol);
        check_failures++;
        return;
    }
    description = ((describe_function)describer)();
    if (weivel_standard_description_check(description, &refusal) != WEIVEL_STATUS_OK) {
        fprintf(stderr, "%s is refused at \"%s\": %s\n", symbol, refusal.path == NULL ? "" : refusal.path,
                refusal.detail == NULL ? "" : refusal.detail);
        weivel_standard_refusal_release(&refusal);
        check_failures++;
        return;
    }
    check_definition_name(description->definition, service_id);
    check_answers(runtime, description, service);
}

static void every_declared_description_is_exported_and_checks(void) {
    weivel_standard_library library = WEIVEL_INIT(weivel_standard_library);
    library_handle runtime = load_runtime();
    CHECK(runtime != NULL);
    if (runtime == NULL) {
        fprintf(stderr, "could not load %s\n", RUNTIME_LIBRARY_PATH);
        return;
    }
    if (read_library(&library)) {
        CHECK(library.service_count > 0);
        for (size_t index = 0; index < library.service_count; index++) {
            check_description(runtime, library.descriptions[index], &library.services[index]);
        }
        weivel_standard_library_release(&library);
    }
    unload_runtime(runtime);
}

int main(void) {
    every_declared_description_is_exported_and_checks();
    return check_summary("description_test");
}
