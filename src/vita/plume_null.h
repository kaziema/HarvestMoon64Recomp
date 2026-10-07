#pragma once

#include <cstdint>

// Null Plume backend for the Vita (Phase 0 step 2).
//
// RT64 runs its whole front end on it and every GPU command is accepted and dropped, so RT64's CPU cost can be
// measured with no GPU work. Buffers that RT64 maps get real memory, because RT64 writes its uploads into them.
// The counters show how much GPU work RT64 asks for, which sizes the GXM backend that replaces this in Phase 1.

namespace hm64vita {
    struct PlumeNullStats {
        uint32_t command_lists_executed;
        uint32_t draws;
        uint32_t dispatches;
        uint32_t copies;
        uint32_t presents;
        uint32_t buffers_alive;
        uint32_t textures_alive;
        uint64_t mapped_bytes; // memory currently allocated for mapped buffers
    };

    PlumeNullStats get_plume_null_stats();
}
