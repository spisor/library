#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/library.h>

#include "check.h"
#include "declaration_file.h"
#include "definition_symbols.h"

/**
 * @file
 * Reads `runtime/library.json` with the weivel standard's own reader and requires it to declare runtime 0.2.0 with its
 * release, and exactly one service per `runtime_*_definition_of` a header under `include/runtime` declares: named by
 * the symbol's middle, or `weivel.<name>` for the middle `standard_<name>` of a standard service, holding that symbol
 * as its one function of the definition kind and declaring the `_description` beside it as its description. The
 * symbols come from the headers at configure time, so a service added to a header and left out of the declaration
 * fails here, and every header including `runtime/definition.h` must declare a getter, so a declaration the configure
 * step failed to read fails too. Each header must also declare exactly one
 * `<struct>_of` getter per `runtime_*_definition` struct it defines, so a second definition added beside a getter's
 * is not left out of everything the getters reach.
 */

static const char *const header_symbols[] = {RUNTIME_DEFINITION_SYMBOLS};
static const size_t header_symbol_count = sizeof header_symbols / sizeof header_symbols[0];

static const char *const header_bodies[] = {RUNTIME_DEFINITION_BODIES};
static const int getter_header_count = RUNTIME_DEFINITION_GETTER_HEADER_COUNT;
static const char *const header_getters[] = {RUNTIME_DEFINITION_GETTERS};
static const size_t header_body_count = sizeof header_bodies / sizeof header_bodies[0];
static const size_t header_getter_count = sizeof header_getters / sizeof header_getters[0];

static const char definition_prefix[] = "runtime_";
static const char definition_suffix[] = "_definition_of";
static const char description_suffix[] = "_definition_description";
static const char standard_prefix[] = "weivel.";

static int read_library(weivel_standard_library *library) {
    size_t length = 0;
    char *text = read_declaration_file(&length);
    weivel_standard_refusal refusal = WEIVEL_INIT(weivel_standard_refusal);
    weivel_status status;
    CHECK(text != NULL);
    if (text == NULL) {
        fprintf(stderr, "could not read %s\n", RUNTIME_DECLARATION_PATH);
        return 0;
    }
    status = weivel_standard_read_library(text, length, library, &refusal);
    free(text);
    if (status != WEIVEL_STATUS_OK) {
        fprintf(stderr, "the declaration was refused at \"%s\": %s\n", refusal.path == NULL ? "" : refusal.path,
                refusal.detail == NULL ? "" : refusal.detail);
        weivel_standard_refusal_release(&refusal);
        check_failures++;
        return 0;
    }
    return 1;
}

/** The symbols' middle for a service id: the id itself, or `standard_<name>` for the standard's `weivel.<name>`. */
static void middle_of(const char *service_id, char *middle, size_t capacity) {
    size_t prefix_length = sizeof standard_prefix - 1;
    if (strncmp(service_id, standard_prefix, prefix_length) == 0) {
        snprintf(middle, capacity, "standard_%s", service_id + prefix_length);
    } else {
        snprintf(middle, capacity, "%s", service_id);
    }
}

static int is_symbol_of(const char *symbol, const char *service_id, const char *suffix) {
    char middle[128];
    char expected[192];
    middle_of(service_id, middle, sizeof middle);
    snprintf(expected, sizeof expected, "%s%s%s", definition_prefix, middle, suffix);
    return symbol != NULL && strcmp(symbol, expected) == 0;
}

static void check_description(const char *description, const char *service_id) {
    char middle[128];
    if (!is_symbol_of(description, service_id, description_suffix)) {
        middle_of(service_id, middle, sizeof middle);
        fprintf(stderr, "service %s declares the description %s, not runtime_%s_definition_description\n", service_id,
                description == NULL ? "(none)" : description, middle);
        check_failures++;
    }
}

static void check_service(const weivel_standard_library_service *service) {
    const weivel_standard_library_function *function = &service->functions[0];
    if (service->function_count != 1) {
        fprintf(stderr, "service %s declares %u functions, not its one definition\n", service->id,
                (unsigned)service->function_count);
        check_failures++;
        return;
    }
    check_text(function->name, "definition", service->id);
    CHECK(function->kind == WEIVEL_STANDARD_FUNCTION_DEFINITION && function->context == 0);
    if (!is_symbol_of(function->symbol, service->id, definition_suffix)) {
        char middle[128];
        middle_of(service->id, middle, sizeof middle);
        fprintf(stderr, "service %s names the symbol %s, not runtime_%s_definition_of\n", service->id,
                function->symbol, middle);
        check_failures++;
    }
}

static int declares_symbol(const weivel_standard_library *library, const char *symbol) {
    for (size_t index = 0; index < library->service_count; index++) {
        const weivel_standard_library_service *service = &library->services[index];
        if (service->function_count == 1 && strcmp(service->functions[0].symbol, symbol) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_header_symbol(const char *symbol) {
    for (size_t index = 0; index < header_symbol_count; index++) {
        if (strcmp(header_symbols[index], symbol) == 0) {
            return 1;
        }
    }
    return 0;
}

static void every_header_definition_is_declared_and_nothing_else(const weivel_standard_library *library) {
    CHECK(getter_header_count == RUNTIME_DEFINITION_HEADER_COUNT);
    for (size_t index = 0; index < header_symbol_count; index++) {
        if (!declares_symbol(library, header_symbols[index])) {
            fprintf(stderr, "%s is declared by a header but not by runtime/library.json\n", header_symbols[index]);
            check_failures++;
        }
    }
    for (size_t index = 0; index < library->service_count; index++) {
        const weivel_standard_library_service *service = &library->services[index];
        check_service(service);
        check_description(library->descriptions[index], service->id);
        if (service->function_count == 1 && !is_header_symbol(service->functions[0].symbol)) {
            fprintf(stderr, "%s is declared but no header under include/runtime declares it\n",
                    service->functions[0].symbol);
            check_failures++;
        }
    }
    CHECK(library->service_count == header_symbol_count);
}

static int lists(const char *const *entries, size_t count, const char *entry) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(entries[index], entry) == 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether a getter entry is a standard service's, whose struct is the standard's and defined in no header here. */
static int is_standard_getter(const char *entry) {
    return strstr(entry, " runtime_standard_") != NULL;
}

/**
 * Each entry is `<header> <name>`, so the getter of a struct in the same header is that entry plus `_of`; every getter
 * but a standard service's has its struct.
 */
static void every_definition_struct_has_its_getter_beside_it(void) {
    size_t standard_getters = 0;
    for (size_t index = 0; index < header_getter_count; index++) {
        standard_getters += is_standard_getter(header_getters[index]) ? 1 : 0;
    }
    CHECK(header_body_count + standard_getters == header_getter_count);
    for (size_t index = 0; index < header_body_count; index++) {
        char getter[192];
        int written = snprintf(getter, sizeof getter, "%s_of", header_bodies[index]);
        CHECK(written > 0 && (size_t)written < sizeof getter);
        if (!lists(header_getters, header_getter_count, getter)) {
            fprintf(stderr, "%s defines a definition and declares no %s getter beside it\n", header_bodies[index],
                    getter);
            check_failures++;
        }
    }
}

static void the_declaration_reads_in_the_standards_format(void) {
    weivel_standard_library library = WEIVEL_INIT(weivel_standard_library);
    if (!read_library(&library)) {
        return;
    }
    check_text(library.import_name, "runtime", "importName");
    check_text(library.version, "0.2.0", "version");
    CHECK(library.release == NULL);
    every_header_definition_is_declared_and_nothing_else(&library);
    weivel_standard_library_release(&library);
}

int main(void) {
    the_declaration_reads_in_the_standards_format();
    every_definition_struct_has_its_getter_beside_it();
    return check_summary("declaration_test");
}
