#ifndef HOST_TEST_C_FIXTURE_SERVICE_H
#define HOST_TEST_C_FIXTURE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include <weivel/value.h>

/** The symbol the C fixture plugin exports its weivel_c_entry_provider under, which the test host programs read. */
#define C_FIXTURE_ENTRY_SYMBOL "c_fixture_plugin_entry"

/** The environment variable naming the file each activation of a C fixture plugin appends "<import name> <pid>" to. */
#define C_FIXTURE_ACTIVATIONS_VARIABLE "SPISOR_FIXTURE_ACTIVATIONS"

/**
 * The object a C fixture plugin registers under each service id its manifest provides: the test host programs'
 * realisation of a method. `call` runs `method` with `arguments`, writing the result's text into `result`, and
 * answers 1, or 0 with `result` holding why it failed.
 *
 * @implNote weivel/c names no realisation of a method, since it ships no host program; this one is the test hosts'.
 */
typedef struct c_fixture_method_service {
    uint32_t size;
    void *data;
    int (*call)(void *data, const char *method, const weivel_standard_value *arguments, char *result,
                size_t capacity);
} c_fixture_method_service;

#endif
