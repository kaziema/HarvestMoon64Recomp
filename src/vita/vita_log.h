#pragma once

// Crash-safe diagnostic log for the Vita build: ux0:data/hm64/phase0.log
// Every line is opened, appended and closed straight away, so the log survives a crash.

namespace hm64vita {
    void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Logs free user, CDRAM and phycont memory, tagged with where it was taken.
    void log_memory(const char* where);
}
