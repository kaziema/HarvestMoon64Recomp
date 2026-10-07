#include "vita_timeline.h"

#include <atomic>
#include <cstdio>
#include <cstdint>

#include <psp2/kernel/processmgr.h>

#include "vita_log.h"

namespace {
    struct Entry {
        uint32_t time_us; // relative to the capture start
        hm64vita::TimelineEvent event;
    };

    constexpr uint32_t MaxEntries = 1024;
    constexpr uint64_t CaptureLengthUs = 2000000;

    Entry entries[MaxEntries];
    std::atomic<uint32_t> entry_count{0};
    std::atomic<bool> capturing{false};
    std::atomic<bool> done{false};
    uint64_t capture_start_us = 0;

    const char* event_name(hm64vita::TimelineEvent event) {
        switch (event) {
            case hm64vita::TimelineEvent::AudioBegin: return "aud+";
            case hm64vita::TimelineEvent::AudioEnd: return "aud-";
            case hm64vita::TimelineEvent::DisplayListBegin: return "dl+";
            case hm64vita::TimelineEvent::DisplayListEnd: return "dl-";
            case hm64vita::TimelineEvent::VI: return "vi";
            case hm64vita::TimelineEvent::ScreenUpdate: return "scr";
        }
        return "?";
    }
}

void hm64vita::timeline_event(TimelineEvent event) {
    if (!capturing.load(std::memory_order_relaxed)) {
        return;
    }
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now - capture_start_us >= CaptureLengthUs) {
        capturing = false;
        return;
    }
    const uint32_t index = entry_count.fetch_add(1);
    if (index >= MaxEntries) {
        capturing = false;
        return;
    }
    entries[index] = { uint32_t(now - capture_start_us), event };
}

void hm64vita::timeline_tick(unsigned int second, unsigned int start_second) {
    if (done) {
        return;
    }
    if (second == start_second) {
        capture_start_us = sceKernelGetProcessTimeWide();
        capturing = true;
        log_line("timeline: capturing %u ms of events (aud = audio task, dl = RT64 display list, + begin, - end)", (unsigned int)(CaptureLengthUs / 1000));
        return;
    }
    if (second < start_second + 3) {
        return;
    }

    capturing = false;
    done = true;
    const uint32_t count = entry_count.load() < MaxEntries ? entry_count.load() : MaxEntries;
    char line[480];
    int length = 0;
    for (uint32_t i = 0; i < count; i++) {
        const int written = snprintf(line + length, sizeof(line) - length, " %u.%u:%s", (unsigned int)(entries[i].time_us / 1000),
            (unsigned int)((entries[i].time_us % 1000) / 100), event_name(entries[i].event));
        if (written <= 0 || written >= (int)(sizeof(line) - length)) {
            break;
        }
        length += written;
        if (length > (int)sizeof(line) - 40 || i + 1 == count) {
            log_line("timeline (ms:event):%s", line);
            length = 0;
        }
    }
    log_line("timeline: %u events logged", (unsigned int)count);
}
