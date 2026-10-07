// Audio output for the Vita: the game's stereo samples, resampled to 48 kHz and played on the main audio port.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hm64vita {
    // Opens the audio port and starts the output thread. Safe to call once at startup.
    void audio_init();

    // The ultramodern audio callbacks.
    void audio_queue_samples(const int16_t* samples, size_t sample_count);
    size_t audio_frames_remaining();
    void audio_set_frequency(uint32_t freq);
}
