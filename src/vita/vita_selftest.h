#pragma once

// Startup checks of the threading primitives ultramodern depends on (TEST 5 hang hypotheses).
// Runs in about half a second before the game starts and writes every result to the log.

namespace hm64vita {
    void run_threading_self_test();
}
