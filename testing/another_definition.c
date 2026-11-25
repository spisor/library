#include "another_definition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <weivel/c/host.h>
#include <weivel/c/host/library.h>
#include <weivel/c/standard.h>

/**
 * Points the definition function of the service `id` among `services` at `symbol`, through a copy of its functions
 * the caller frees; NULL when no such service declares a definition, or out of memory.
 */
static weivel_standard_library_function *serve_by(weivel_standard_library_service *services, size_t service_count,
                                                  const char *id, const char *symbol) {
    for (size_t index = 0; index < service_count; index++) {
        weivel_standard_library_service *service = &services[index];
        weivel_standard_library_function *functions;
        if (strcmp(service->id, id) != 0) {
            continue;
        }
        functions = malloc(service->function_count * sizeof *functions);
        if (functions == NULL) {
            return NULL;
        }
        memcpy(functions, service->functions, service->function_count * sizeof *functions);
        for (size_t function = 0; function < service->function_count; function++) {
            if (functions[function].kind == WEIVEL_STANDARD_FUNCTION_DEFINITION) {
                functions[function].symbol = symbol;
                service->functions = functions;
                return functions;
            }
        }
        free(functions);
    }
    return NULL;
}

static size_t check_load_refused(const char *core_path, const char *library_path,
                                 const weivel_standard_library *declaration, const char *refused_description) {
    weivel_c_host_options options = WEIVEL_INIT(weivel_c_host_options);
    weivel_c_core_load_failure core_failure = WEIVEL_INIT(weivel_c_core_load_failure);
    weivel_c_library_load_failure failure = WEIVEL_INIT(weivel_c_library_load_failure);
    weivel_c_host *host = NULL;
    weivel_c_library *library = NULL;
    weivel_c_status status;
    size_t failures = 0;
    options.core_path = core_path;
    options.description_check.check = weivel_c_standard_check_description;
    if (weivel_c_host_create(&options, &host, &core_failure) != WEIVEL_C_STATUS_OK || host == NULL) {
        fprintf(stderr, "no host could be created over %s\n", core_path);
        return 1;
    }
    status = weivel_c_host_load_library(host, library_path, declaration, &library, &failure, NULL);
    if (status != WEIVEL_C_STATUS_MISUSE || library != NULL) {
        fprintf(stderr, "%s served by another definition was not refused: status %d\n", library_path, (int)status);
        failures++;
    }
    if (failure.refused_description == NULL || strcmp(failure.refused_description, refused_description) != 0) {
        fprintf(stderr, "the description refused: expected \"%s\", got \"%s\"\n", refused_description,
                failure.refused_description == NULL ? "(null)" : failure.refused_description);
        failures++;
    }
    if (weivel_c_host_destroy(host, NULL) != WEIVEL_C_STATUS_OK || weivel_c_host_free(host) != WEIVEL_C_STATUS_OK) {
        fprintf(stderr, "the host over %s was not destroyed and freed\n", core_path);
        failures++;
    }
    return failures;
}

size_t spisor_check_served_by_another_definition(const char *core_path, const char *library_path,
                                                 const weivel_standard_library *declaration, const char *service_id,
                                                 const char *definition_symbol, const char *refused_description) {
    weivel_standard_library swapped = *declaration;
    weivel_standard_library_service *services = malloc(declaration->service_count * sizeof *services);
    weivel_standard_library_function *functions = NULL;
    size_t failures;
    if (services == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    memcpy(services, declaration->services, declaration->service_count * sizeof *services);
    functions = serve_by(services, declaration->service_count, service_id, definition_symbol);
    if (functions == NULL) {
        fprintf(stderr, "no service %s declares a definition to point elsewhere, or out of memory\n", service_id);
        free(services);
        return 1;
    }
    swapped.services = services;
    failures = check_load_refused(core_path, library_path, &swapped, refused_description);
    free(functions);
    free(services);
    return failures;
}
