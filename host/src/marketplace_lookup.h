#ifndef HOST_MARKETPLACE_LOOKUP_H
#define HOST_MARKETPLACE_LOOKUP_H

/**
 * The apiBase marketplace build passes to its lookups: NULL, so each host's own API address is used.
 *
 * @implNote Only the test build of this file can answer anything else, through host_marketplace_aim_lookups, which
 * the production build does not define; the runtime refuses an apiBase unless its own fetch test hooks are built in.
 */
const char *host_marketplace_lookup_api_base(void);

#if defined(HOST_MARKETPLACE_TEST_HOOK)
/** Aims the lookups at api_base; NULL aims them at the hosts again. */
void host_marketplace_aim_lookups(const char *api_base);
#endif

#endif
