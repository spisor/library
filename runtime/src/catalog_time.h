#ifndef RUNTIME_CATALOG_TIME_H
#define RUNTIME_CATALOG_TIME_H

#define RUNTIME_CATALOG_TIME_SIZE 25

/**
 * The milliseconds since the epoch of an ISO 8601 date and time with Z or an offset, as Date.parse reads
 * what toISOString writes. 1 on success, 0 for any other text.
 */
int runtime_catalog_parse_time(const char *text, double *milliseconds);

/** toISOString for a time in the years 0 to 9999: 1 on success, 0 outside them. */
int runtime_catalog_format_time(double milliseconds, char out[RUNTIME_CATALOG_TIME_SIZE]);

#endif
