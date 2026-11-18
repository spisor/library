#ifndef RUNTIME_CATALOG_URL_HOST_H
#define RUNTIME_CATALOG_URL_HOST_H

#include <stddef.h>

#include "spisor/status.h"

/**
 * The WHATWG URL host parser for a special url's host, serialized: an IPv6 address in brackets, an IPv4
 * address in dotted decimal, or a lowercase domain, in *host, freed: SPISOR_MALFORMED when the host is not one,
 * SPISOR_OUT_OF_MEMORY when memory runs out, *host NULL for both.
 *
 * @implNote A domain with a letter outside ASCII is refused here rather than converted, since no IDNA
 * mapping is carried; Node would convert it to punycode. An xn-- label is taken as it is written.
 */
spisor_status runtime_catalog_parse_host(const char *bytes, size_t length, char **host);

#endif
