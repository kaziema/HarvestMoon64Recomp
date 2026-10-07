// Declarations RT64's dependencies expect from the C library that newlib on the Vita does not provide.
// Force-included into the RT64 sources that need them (see vita/rt64.cmake); defined in src/vita/vita_rt64_compat.cpp.
#pragma once

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// UTC version of mktime (used by implot's time axis).
time_t timegm(struct tm *tm);

#ifdef __cplusplus
}
#endif
