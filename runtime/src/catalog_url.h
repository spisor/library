#ifndef RUNTIME_CATALOG_URL_H
#define RUNTIME_CATALOG_URL_H

#include "spisor/status.h"

/**
 * url resolved against origin, the index's own url, as new URL(url, origin).href, when it stays within
 * the kind of source that served the index: any https url under an https index, and under a file index a
 * file url inside the index's own directory with no host, in *href, freed: SPISOR_REFUSED otherwise,
 * SPISOR_OUT_OF_MEMORY when memory runs out, *href NULL for both.
 *
 * @implNote A catalog is untrusted input, so a hostile index cannot make the host fetch from another
 * scheme, a UNC share or a sibling directory. The resolution is the WHATWG URL parser for the two
 * schemes it can answer, pinned against Node by test/fixtures/catalog/urls.json.
 */
spisor_status runtime_catalog_resolve_within_origin(const char *url, const char *origin, char **href);

/**
 * Node's pathToFileURL(path).href for an absolute path, as on Windows when windows is nonzero, in *url, freed:
 * SPISOR_MALFORMED for a path no file url names, such as a UNC path without a host, SPISOR_OUT_OF_MEMORY when
 * memory runs out, *url NULL for both.
 */
spisor_status runtime_catalog_file_url_of_path(int windows, const char *path, char **url);

#endif
