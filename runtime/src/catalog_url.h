#ifndef RUNTIME_CATALOG_URL_H
#define RUNTIME_CATALOG_URL_H

/**
 * url resolved against origin, the index's own url, as new URL(url, origin).href, when it stays within
 * the kind of source that served the index: any https url under an https index, and under a file index a
 * file url inside the index's own directory with no host. NULL otherwise, or when memory runs out.
 *
 * @implNote A catalog is untrusted input, so a hostile index cannot make the host fetch from another
 * scheme, a UNC share or a sibling directory. The resolution is the WHATWG URL parser for the two
 * schemes it can answer, pinned against Node by test/fixtures/catalog/urls.json.
 */
char *runtime_catalog_resolve_within_origin(const char *url, const char *origin);

/** Node's pathToFileURL(path).href for an absolute path, as on Windows when windows is nonzero. */
char *runtime_catalog_file_url_of_path(int windows, const char *path);

#endif
