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

    // Diagnostics: buffering mode (1 = test 20 behaviour, 0 = test 19 behaviour), and saving the last 20 seconds
    // of game-side and speaker-side audio to ux0:data/hm64/audio_<tag>_*.raw.
    void audio_set_buffering_mode(int mode);
    int audio_buffering_mode();
    void audio_save_recordings(const char* tag);

    // Writes the audio statistics to the log. Called periodically from a thread that is not the output thread.
    void audio_log_stats();
}
