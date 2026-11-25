#ifndef SPISOR_TESTING_ANOTHER_DEFINITION_H
#define SPISOR_TESTING_ANOTHER_DEFINITION_H

/**
 * @file
 * Checks that weivel/c's host, given the standard's check, refuses a library declared with one service's definition
 * function pointed at another's: the description still passes the standard's check, so only the check that a
 * description answers its service's definition refuses the load.
 */

#include <stddef.h>

#include <weivel/library.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Loads the shared library at `library_path` on a fresh host over the core at `core_path`, from `declaration` with the
 * definition function of the service `service_id` pointed at `definition_symbol`, and requires the load to answer
 * WEIVEL_C_STATUS_MISUSE, load nothing, and name `refused_description` as the description it refused. `declaration`
 * is left as it was.
 *
 * @implNote A process should load the library this once: ASan on macOS reports a library loaded again after its
 * unload as an ODR violation of its own globals.
 * @return the failures, each printed.
 */
size_t spisor_check_served_by_another_definition(const char *core_path, const char *library_path,
                                                 const weivel_standard_library *declaration, const char *service_id,
                                                 const char *definition_symbol, const char *refused_description);

#ifdef __cplusplus
}
#endif

#endif
