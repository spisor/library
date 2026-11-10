#include <stddef.h>

#include "marketplace_lookup.h"

#if defined(HOST_MARKETPLACE_TEST_HOOK)
static const char *aimed_api_base = NULL;

void host_marketplace_aim_lookups(const char *api_base) {
    aimed_api_base = api_base;
}

const char *host_marketplace_lookup_api_base(void) {
    return aimed_api_base;
}
#else
const char *host_marketplace_lookup_api_base(void) {
    return 0;
}
#endif
