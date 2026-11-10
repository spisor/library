#ifndef HOST_INSTALL_WARNING_REPORT_H
#define HOST_INSTALL_WARNING_REPORT_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Prints to standard error one line for each way a plugin the install report names, installed now or already
 * cached, depends on something by a name instead of an id, as install.warnings finds them in its plugin.json.
 *
 * @implNote Only a warning: the install has finished, and a report row that is not a plugin, or one whose
 * manifest cannot be read, is skipped without a word.
 */
void host_print_install_warnings(const host_context *context, const cJSON *report);

#endif
