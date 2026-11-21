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
 * the symbol's middle, holding that symbol as its one function of the definition kind and declaring
 * `runtime_<id>_definition_description` as its description. The symbols come from the headers at configure time, so a
 * service added to a header and left out of the declaration fails here.
 */

static const char *const header_symbols[] = {RUNTIME_DEFINITION_SYMBOLS};
static const size_t header_symbol_count = sizeof header_symbols / sizeof header_symbols[0];

static const char definition_prefix[] = "runtime_";
static const char definition_suffix[] = "_definition_of";
static const char description_suffix[] = "_definition_description";

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

static int is_symbol_of(const char *symbol, const char *service_id, const char *suffix) {
    size_t prefix_length = sizeof definition_prefix - 1;
    size_t suffix_length = strlen(suffix);
    size_t id_length = strlen(service_id);
    return symbol != NULL && strlen(symbol) == prefix_length + id_length + suffix_length &&
           strncmp(symbol, definition_prefix, prefix_length) == 0 &&
           strncmp(symbol + prefix_length, service_id, id_length) == 0 &&
           strcmp(symbol + prefix_length + id_length, suffix) == 0;
}

static void check_description(const char *description, const char *service_id) {
    if (!is_symbol_of(description, service_id, description_suffix)) {
        fprintf(stderr, "service %s declares the description %s, not runtime_%s_definition_description\n", service_id,
                description == NULL ? "(none)" : description, service_id);
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
        fprintf(stderr, "service %s names the symbol %s, not runtime_%s_definition_of\n", service->id,
                function->symbol, service->id);
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
    CHECK(header_symbol_count == RUNTIME_DEFINITION_HEADER_COUNT);
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
    return check_summary("declaration_test");
}
