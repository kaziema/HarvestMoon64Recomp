// C library functions RT64's dependencies expect that newlib on the Vita does not provide.
// Declared in vita/rt64/vita_rt64_compat.h.

#include "vita_rt64_compat.h"

#include <cstdint>

// UTC version of mktime: days since 1970-01-01 from the civil date (Howard Hinnant's algorithm), plus the time of day.
extern "C" time_t timegm(struct tm *tm) {
    int64_t year = int64_t(tm->tm_year) + 1900;
    int64_t month = int64_t(tm->tm_mon) + 1;
    year += (month - 1) / 12;
    month = (month - 1) % 12 + 1;
    if (month <= 0) {
        month += 12;
        year -= 1;
    }
    year -= (month <= 2) ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const int64_t days = era * 146097 + day_of_era - 719468;
    return time_t(days * 86400 + int64_t(tm->tm_hour) * 3600 + int64_t(tm->tm_min) * 60 + tm->tm_sec);
}
