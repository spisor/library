#ifndef RUNTIME_ALIVE_STAMP_H
#define RUNTIME_ALIVE_STAMP_H

#include "cJSON.h"

/**
 * Whether the process a stamp names still runs: its pid is alive and it said so at alive_at, within three
 * of its check_milliseconds of now. A NaN pid or alive_at is never fresh; a NaN check_milliseconds is the
 * default interval, RUNTIME_DEFAULT_CHECK_MILLISECONDS.
 *
 * @implNote The stamp is what tells a running process from an unrelated one given its pid after it died,
 * which a pid alone cannot. A background host's holders record and a start's run record are judged alike.
 */
int runtime_is_stamp_fresh(double pid, double alive_at, double check_milliseconds, double now);

/** The finite number member of record, or NaN when it is absent, null or not a finite number. */
double runtime_number_or_nan(const cJSON *record, const char *member);

#endif
