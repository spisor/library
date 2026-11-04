#include "catalog_time.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MILLISECONDS_PER_DAY 86400000.0
#define MILLISECONDS_PER_MINUTE 60000.0

typedef struct civil_time {
    long year;
    unsigned month;
    unsigned day;
    unsigned hour;
    unsigned minute;
    unsigned second;
    unsigned millisecond;
} civil_time;

static int digits_at(const char *text, size_t count, unsigned *value) {
    *value = 0;
    for (size_t index = 0; index < count; index++) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        *value = *value * 10 + (unsigned)(text[index] - '0');
    }
    return 1;
}

static int is_leap(long year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(long year, unsigned month) {
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : days[month - 1];
}

/** Days from 1970-01-01 to the civil date, by Howard Hinnant's days_from_civil. */
static long days_from_civil(long year, unsigned month, unsigned day) {
    year -= month <= 2;
    long era = (year >= 0 ? year : year - 399) / 400;
    long year_of_era = year - era * 400;
    long day_of_year = (153 * (long)(month > 2 ? month - 3 : month + 9) + 2) / 5 + (long)day - 1;
    long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static void civil_from_days(long days, civil_time *time) {
    days += 719468;
    long era = (days >= 0 ? days : days - 146096) / 146097;
    long day_of_era = days - era * 146097;
    long year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    long day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    long month_index = (5 * day_of_year + 2) / 153;
    time->day = (unsigned)(day_of_year - (153 * month_index + 2) / 5 + 1);
    time->month = (unsigned)(month_index < 10 ? month_index + 3 : month_index - 9);
    time->year = year_of_era + era * 400 + (time->month <= 2);
}

static int read_fraction(const char **at, unsigned *millisecond) {
    *millisecond = 0;
    if (**at != '.') {
        return 1;
    }
    (*at)++;
    unsigned scale = 100;
    size_t count = 0;
    while (**at >= '0' && **at <= '9') {
        *millisecond += (unsigned)(**at - '0') * scale;
        scale /= 10;
        (*at)++;
        count++;
    }
    return count > 0;
}

static int read_offset(const char *at, double *offset_minutes) {
    if (strcmp(at, "Z") == 0) {
        *offset_minutes = 0;
        return 1;
    }
    unsigned hours;
    unsigned minutes;
    if ((at[0] != '+' && at[0] != '-') || !digits_at(at + 1, 2, &hours) || at[3] != ':' ||
        !digits_at(at + 4, 2, &minutes) || at[6] != 0 || hours > 23 || minutes > 59) {
        return 0;
    }
    *offset_minutes = (at[0] == '-' ? -1.0 : 1.0) * (hours * 60.0 + minutes);
    return 1;
}

static int read_civil(const char *text, civil_time *time, const char **rest) {
    unsigned year;
    if (!digits_at(text, 4, &year) || text[4] != '-' || !digits_at(text + 5, 2, &time->month) || text[7] != '-' ||
        !digits_at(text + 8, 2, &time->day) || text[10] != 'T' || !digits_at(text + 11, 2, &time->hour) ||
        text[13] != ':' || !digits_at(text + 14, 2, &time->minute) || text[16] != ':' ||
        !digits_at(text + 17, 2, &time->second)) {
        return 0;
    }
    time->year = (long)year;
    *rest = text + 19;
    return time->month >= 1 && time->month <= 12 && time->day >= 1 &&
           time->day <= days_in_month(time->year, time->month) && time->hour <= 23 && time->minute <= 59 &&
           time->second <= 59 && read_fraction(rest, &time->millisecond);
}

int runtime_catalog_parse_time(const char *text, double *milliseconds) {
    civil_time time;
    const char *rest;
    double offset_minutes;
    if (strlen(text) < 20 || !read_civil(text, &time, &rest) || !read_offset(rest, &offset_minutes)) {
        return 0;
    }
    double day_start = (double)days_from_civil(time.year, time.month, time.day) * MILLISECONDS_PER_DAY;
    *milliseconds = day_start + time.hour * 3600000.0 + time.minute * MILLISECONDS_PER_MINUTE + time.second * 1000.0 +
                    time.millisecond - offset_minutes * MILLISECONDS_PER_MINUTE;
    return 1;
}

int runtime_catalog_format_time(double milliseconds, char out[RUNTIME_CATALOG_TIME_SIZE]) {
    if (!isfinite(milliseconds)) {
        return 0;
    }
    double days = floor(milliseconds / MILLISECONDS_PER_DAY);
    double within_day = milliseconds - days * MILLISECONDS_PER_DAY;
    civil_time time;
    civil_from_days((long)days, &time);
    if (time.year < 0 || time.year > 9999) {
        return 0;
    }
    unsigned long remaining = (unsigned long)within_day;
    time.millisecond = (unsigned)(remaining % 1000);
    time.second = (unsigned)(remaining / 1000 % 60);
    time.minute = (unsigned)(remaining / 60000 % 60);
    time.hour = (unsigned)(remaining / 3600000);
    snprintf(out, RUNTIME_CATALOG_TIME_SIZE, "%04ld-%02u-%02uT%02u:%02u:%02u.%03uZ", time.year, time.month, time.day,
             time.hour, time.minute, time.second, time.millisecond);
    return 1;
}
