#ifndef HOST_INSTALL_BY_NAME_H
#define HOST_INSTALL_BY_NAME_H

#include "host_context.h"

/** What `spisor-host install --name` was asked; name and application are required, the accepted ones are optional. */
typedef struct host_name_request {
    const char *name;
    const char *application;
    const char *accepted_name_id;
    const char *accepted_primary;
    const char *accepted_number;
} host_name_request;

/**
 * Installs the plugin request->name means, a bare name or marketplace/name, out of the marketplaces the portable settings
 * list, into the plugin folder of the registered application request->application, and prints what install.byId answered
 * and then the warnings install.warnings finds in the plugin's manifest. The exit code: 0 done, 1 refused. Installs
 * nothing when the name is found nowhere, is found under two ids, or is a name this host remembers as another id.
 *
 * accepted_name_id is the id the user accepted in place of the one remembered for the name, accepted_primary the
 * primary the user accepted in place of the one confirmed for the repo, and accepted_number the place the user accepted
 * as a disputed account's number; each changes nothing unless it is what is now offered, and none is ever taken for
 * another.
 *
 * @implNote A name is remembered only once its install has succeeded, so a failed first install records nothing that
 * a later prompt could claim was installed. A bare name is remembered under the bare name as well as under its marketplace,
 * since a name that moves to another marketplace is no change to the qualified key it never had.
 *
 * No host runs a plugin deployed this way yet: hosts move to deployed plugins in plan four.
 */
int host_install_by_name(const host_context *context, const host_name_request *request);

#endif
