#include "described_checks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/library.h>

#include "erased_calls.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef void (*exported_function)(void);
typedef const weivel_standard_description *(*describe_function)(void);
typedef const void *(*definition_function)(void);

#ifdef _WIN32
typedef HMODULE library_handle;

static library_handle load_library(const char *path) {
    return LoadLibraryA(path);
}

static exported_function symbol_of(library_handle library, const char *symbol) {
    return (exported_function)GetProcAddress(library, symbol);
}

static void unload_library(library_handle library) {
    FreeLibrary(library);
}
#else
typedef void *library_handle;

static library_handle load_library(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static exported_function symbol_of(library_handle library, const char *symbol) {
    void *address = dlsym(library, symbol);
    exported_function function = NULL;
    if (address != NULL) {
        memcpy(&function, &address, sizeof function);
    }
    return function;
}

static void unload_library(library_handle library) {
    dlclose(library);
}
#endif

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *text = NULL;
    long end;
    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (end = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0) {
        text = malloc((size_t)end + 1);
        if (text != NULL && fread(text, 1, (size_t)end, file) != (size_t)end) {
            free(text);
            text = NULL;
        }
        *length = (size_t)end;
    }
    fclose(file);
    return text;
}

static int read_declaration(const char *path, weivel_standard_library *declaration) {
    size_t length = 0;
    char *text = read_file(path, &length);
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    weivel_status status;
    if (text == NULL) {
        fprintf(stderr, "could not read %s\n", path);
        return 0;
    }
    status = weivel_standard_read_library(text, length, declaration, &refusal);
    free(text);
    if (status != WEIVEL_STATUS_OK) {
        fprintf(stderr, "%s is refused at \"%s\": %s\n", path, refusal.path == NULL ? "" : refusal.path,
                refusal.detail == NULL ? "" : refusal.detail);
        weivel_standard_refusal_release(&refusal);
        return 0;
    }
    return 1;
}

static const char *definition_symbol_of(const weivel_standard_library_service *service) {
    for (size_t index = 0; index < service->function_count; index++) {
        if (service->functions[index].kind == WEIVEL_STANDARD_FUNCTION_DEFINITION) {
            return service->functions[index].symbol;
        }
    }
    return NULL;
}

SPISOR_CALLS_THROUGH_ERASED_TYPES
static void answer_both(describe_function describe, definition_function define, spisor_described_definition *found) {
    found->description = describe();
    found->definition = define();
}

/**
 * Whether the description answers the definition its service's getter answered. A description the standard's check
 * refuses is left to spisor_check_described, which names that refusal.
 */
static int answers_its_definition(const char *service_id, const spisor_described_definition *found) {
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    int answers = 1;
    if (weivel_standard_description_check(found->description, &refusal) == WEIVEL_STATUS_OK &&
        weivel_standard_description_answers(found->description, found->definition, &refusal) != WEIVEL_STATUS_OK) {
        fprintf(stderr, "service %s: its description does not answer its definition, at \"%s\": %s\n", service_id,
                refusal.path == NULL ? "" : refusal.path, refusal.detail == NULL ? "" : refusal.detail);
        answers = 0;
    }
    weivel_standard_refusal_release(&refusal);
    return answers;
}

/** Fills `found` from one service's two symbols; 0, the reason printed, when either is not there to call. */
static int described_definition_of(library_handle library, const weivel_standard_library_service *service,
                                   const char *description_symbol, spisor_described_definition *found) {
    const char *definition_symbol = definition_symbol_of(service);
    describe_function describe;
    definition_function define;
    if (description_symbol == NULL) {
        fprintf(stderr, "service %s has a definition and declares no description, so it goes unchecked\n",
                service->id);
        return 0;
    }
    describe = (describe_function)symbol_of(library, description_symbol);
    define = (definition_function)symbol_of(library, definition_symbol);
    if (describe == NULL || define == NULL) {
        fprintf(stderr, "service %s: %s or %s is not exported\n", service->id, description_symbol, definition_symbol);
        return 0;
    }
    answer_both(describe, define, found);
    if (found->description == NULL || found->definition == NULL) {
        fprintf(stderr, "service %s answers no description or no definition\n", service->id);
        return 0;
    }
    return answers_its_definition(service->id, found);
}

static size_t check_declared(library_handle library, const weivel_standard_library *declaration,
                             spisor_described_check check) {
    spisor_described_definition *definitions = calloc(declaration->service_count + 1, sizeof *definitions);
    size_t definition_count = 0;
    size_t failures = 0;
    if (definitions == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (size_t index = 0; index < declaration->service_count; index++) {
        const weivel_standard_library_service *service = &declaration->services[index];
        if (definition_symbol_of(service) == NULL) {
            continue;
        }
        if (described_definition_of(library, service, declaration->descriptions[index],
                                    &definitions[definition_count])) {
            definition_count++;
        } else {
            failures++;
        }
    }
    failures += spisor_check_described(definitions, definition_count, check);
    free(definitions);
    return failures;
}

size_t spisor_check_library(const char *library_path, const char *declaration_path, spisor_described_check check) {
    weivel_standard_library declaration = WEIVEL_INIT(weivel_standard_library);
    library_handle library;
    size_t failures;
    if (!read_declaration(declaration_path, &declaration)) {
        return 1;
    }
    library = load_library(library_path);
    if (library == NULL) {
        fprintf(stderr, "could not load %s\n", library_path);
        weivel_standard_library_release(&declaration);
        return 1;
    }
    failures = check_declared(library, &declaration, check);
    unload_library(library);
    weivel_standard_library_release(&declaration);
    return failures;
}
