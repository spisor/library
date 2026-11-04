#ifndef RUNTIME_CATALOG_URL_HOST_H
#define RUNTIME_CATALOG_URL_HOST_H

#include <stddef.h>

/**
 * The WHATWG URL host parser for a special url's host, serialized: an IPv6 address in brackets, an IPv4
 * address in dotted decimal, or a lowercase domain. NULL when the host is not one, or memory runs out.
 *
 * @implNote A domain with a letter outside ASCII is refused here rather than converted, since no IDNA
 * mapping is carried; Node would convert it to punycode. An xn-- label is taken as it is written.
 */
char *runtime_catalog_parse_host(const char *bytes, size_t length);

#endif
