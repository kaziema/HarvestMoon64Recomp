#pragma once

#include <stdint.h>

#include "rt64_vita_profile.h"

namespace hm64vita {
    // Running totals per RT64 profiling zone since startup: microseconds and call counts (RT64_ZONE_COUNT entries each).
    void rt64_zone_totals(uint64_t* us, uint32_t* calls);
    const char* rt64_zone_name(int zone);
}
