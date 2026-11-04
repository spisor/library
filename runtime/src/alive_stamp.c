#include "alive_stamp.h"

#include <math.h>

#include "process_alive.h"
#include "run_records.h"

#define MISSED_CHECKS_BEFORE_DEAD 3.0

int runtime_is_stamp_fresh(double pid, double alive_at, double check_milliseconds, double now) {
    if (isnan(pid) || isnan(alive_at) || !runtime_process_is_alive(pid)) {
        return 0;
    }
    double interval = isnan(check_milliseconds) ? RUNTIME_DEFAULT_CHECK_MILLISECONDS : check_milliseconds;
    return now - alive_at <= MISSED_CHECKS_BEFORE_DEAD * interval;
}

double runtime_number_or_nan(const cJSON *record, const char *member) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(record, member);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) ? value->valuedouble : NAN;
}
