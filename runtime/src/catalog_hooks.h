#ifndef RUNTIME_CATALOG_HOOKS_H
#define RUNTIME_CATALOG_HOOKS_H

/**
 * Sends each catalog request for a url starting with from to the same url with from replaced by to, so a
 * test can serve an https source's index from a loopback http fixture while the catalog still reads it as
 * the https url it named; NULL restores direct requests.
 *
 * @implNote Defined only when SPISOR_CATALOG_TEST_HOOKS is set, which only the test's own build of the
 * check sources does, together with fetch's hook for plain http.
 */
void runtime_catalog_redirect_requests(const char *from, const char *to);

#endif
