#pragma once

// Frame timeline (TEST 11): records audio task, display list, VI and screen update events with timestamps for a
// two second window, then the stats thread writes them to the log. Shows whether the audio task, RT64 and the
// game run one after another (a serial chain) or overlap.

namespace hm64vita {
    enum class TimelineEvent : unsigned char {
        AudioBegin,
        AudioEnd,
        DisplayListBegin,
        DisplayListEnd,
        VI,
        ScreenUpdate,
    };

    void timeline_event(TimelineEvent event);

    // Called by the stats thread once a second: starts the capture at start_second, logs it when it is full or done.
    void timeline_tick(unsigned int second, unsigned int start_second);
}
