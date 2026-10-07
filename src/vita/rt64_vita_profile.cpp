// Totals for the profiling zones inside RT64 (rt64-gxm src/vita/rt64_vita_profile.h), read by the stats thread.

#define RT64_VITA_PROFILE
#include "rt64_vita_profile.h"
#include "rt64_vita_profile_totals.h"

#include <atomic>

#include <psp2/kernel/processmgr.h>

namespace {
    std::atomic<uint64_t> zone_us[RT64_ZONE_COUNT];
    std::atomic<uint32_t> zone_calls[RT64_ZONE_COUNT];
}

extern "C" uint64_t rt64_vita_profile_now(void) {
    return sceKernelGetProcessTimeWide();
}

extern "C" void rt64_vita_profile_add(int zone, uint64_t elapsed_us) {
    zone_us[zone] += elapsed_us;
    zone_calls[zone]++;
}

extern "C" void rt64_vita_profile_count(int zone) {
    zone_calls[zone]++;
}

const char* hm64vita::rt64_zone_name(int zone) {
    static const char* const names[RT64_ZONE_COUNT] = {
        "fullSync", "fs.flush", "fs.tilesValidate", "fs.fillPairs", "fs.upload", "fs.uberWait",
        "fs.renderToRAM", "fs.pairTiles", "fs.evictToAdvance", "fs.queueAdvance",
        "setVertex", "loadDrawState", "rdpLoad", "checkRDRAM", "fbHash", "drawTri",
    };
    return (zone >= 0 && zone < RT64_ZONE_COUNT) ? names[zone] : "?";
}

void hm64vita::rt64_zone_totals(uint64_t* us, uint32_t* calls) {
    for (int i = 0; i < RT64_ZONE_COUNT; i++) {
        us[i] = zone_us[i].load();
        calls[i] = zone_calls[i].load();
    }
}
