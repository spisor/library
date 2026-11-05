#ifndef HOST_REQUIRED_PLUGINS_H
#define HOST_REQUIRED_PLUGINS_H

#include "cJSON.h"
#include "host_context.h"

/**
 * Checks that every plugin the loader's standard requires is installed and enabled in the settings of the
 * loader's home. 1 when each is; 0 with *refusal set naming the first that is not and how to fix it, or
 * with none when memory runs out.
 *
 * @implNote Not installed is judged before not enabled, since an enable switch means nothing for a plugin
 * that is not there. The fix is installing again, which installs every required plugin, from a placeholder
 * catalog, because a registration records no catalog source.
 */
int host_check_required_plugins(const host_context *context, const char *loader_id, const char *loader_home,
                                const cJSON *standard, cJSON **refusal);

#endif
