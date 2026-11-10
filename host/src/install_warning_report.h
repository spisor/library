#ifndef HOST_INSTALL_WARNING_REPORT_H
#define HOST_INSTALL_WARNING_REPORT_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Prints to standard error one line for each way a plugin the install report names, installed now or already
 * cached, depends on something by a name instead of an id, as install.warnings finds them in its plugin.json.
 *
 * @implNote Only a warning: the install has finished, and a report row that is not a plugin is skipped. A plugin
 * whose manifest cannot be read or checked says so in one line. Manifest text is printed with its control
 * characters escaped, since a name holding an escape sequence could otherwise overwrite the warning.
 */
void host_print_install_warnings(const host_context *context, const cJSON *report);

#endif
