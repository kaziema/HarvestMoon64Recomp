// Audio output for the Vita. See vita_audio.h.
//
// The game's audio thread hands over interleaved 16-bit stereo at the game's rate (32 kHz for Harvest Moon) with
// left and right swapped (the runtime's address xor). Samples are swapped back, halved in volume like the PC
// version, resampled linearly to 48 kHz and written to a ring buffer. A dedicated thread drains the ring into the
// main audio port (48 kHz only) in fixed blocks; sceAudioOutOutput blocks until the hardware needs more, so the
// thread runs at the hardware's pace. When the ring runs dry the rest of the block is silence (an underrun).
// When the game runs slower than real time (Phase 0: about 20 fps), underruns are expected and the sound stutters.

#include "vita_audio.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "vita_log.h"

namespace {
    constexpr uint32_t OutputRate = 48000;
    constexpr uint32_t BlockFrames = 768;           // 16 ms per hardware block
    constexpr uint32_t RingFrames = 48000 / 2;      // 500 ms of 48 kHz stereo
    constexpr uint32_t MaxBufferedFrames = 48000 / 5; // above 200 ms buffered, incoming audio is dropped

    int16_t ring[RingFrames * 2];
    std::atomic<uint32_t> ring_write{0};  // frames written, ever (wraps with the ring by modulo)
    std::atomic<uint32_t> ring_read{0};   // frames read, ever

    std::atomic<uint32_t> input_rate{32000};
    int port = -1;

    // Resampler state carried across chunks: the last input frame and the fractional position into the next.
    float last_left = 0.0f;
    float last_right = 0.0f;
    float position = 0.0f;

    // Totals, logged every 10 seconds by the output thread.
    std::atomic<uint32_t> stat_chunks{0};
    std::atomic<uint32_t> stat_input_frames{0};
    std::atomic<uint32_t> stat_dropped_frames{0};
    uint32_t stat_output_blocks = 0;
    uint32_t stat_underrun_blocks = 0;
    uint32_t stat_underrun_frames = 0;
    uint32_t stat_output_errors = 0;
    int last_output_error = 0;

    uint32_t buffered_frames() {
        return ring_write.load(std::memory_order_acquire) - ring_read.load(std::memory_order_acquire);
    }

    void* output_thread(void*) {
        sceKernelChangeThreadPriority(0, 65);
        static int16_t block[BlockFrames * 2];
        uint64_t last_log_us = sceKernelGetProcessTimeWide();
        uint32_t log_index = 0;
        while (true) {
            const uint32_t available = buffered_frames();
            const uint32_t take = (available < BlockFrames) ? available : BlockFrames;
            uint32_t read = ring_read.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < take; i++) {
                const uint32_t index = (read + i) % RingFrames;
                block[i * 2 + 0] = ring[index * 2 + 0];
                block[i * 2 + 1] = ring[index * 2 + 1];
            }
            ring_read.store(read + take, std::memory_order_release);
            if (take < BlockFrames) {
                memset(&block[take * 2], 0, (BlockFrames - take) * 2 * sizeof(int16_t));
                stat_underrun_blocks++;
                stat_underrun_frames += BlockFrames - take;
            }

            const int result = sceAudioOutOutput(port, block);
            stat_output_blocks++;
            if (result < 0) {
                stat_output_errors++;
                last_output_error = result;
                sceKernelDelayThread(16000);
            }

            const uint64_t now = sceKernelGetProcessTimeWide();
            if (now - last_log_us >= 10000000) {
                last_log_us = now;
                // Only the first few minutes, then every minute.
                log_index++;
                if (log_index <= 18 || (log_index % 6) == 0) {
                    hm64vita::log_line("audio: input %u Hz, %u chunks, %u input frames, %u dropped (over 200 ms buffered) | output %u blocks, %u underrun blocks (%u silent frames), %u errors (last 0x%08X), %u ms buffered",
                        (unsigned int)input_rate.load(), (unsigned int)stat_chunks.load(), (unsigned int)stat_input_frames.load(),
                        (unsigned int)stat_dropped_frames.load(), (unsigned int)stat_output_blocks, (unsigned int)stat_underrun_blocks,
                        (unsigned int)stat_underrun_frames, (unsigned int)stat_output_errors, (unsigned int)last_output_error,
                        (unsigned int)(buffered_frames() * 1000 / OutputRate));
                }
            }
        }
        return nullptr;
    }
}

void hm64vita::audio_init() {
    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, BlockFrames, OutputRate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        log_line("audio: sceAudioOutOpenPort failed 0x%08X (no sound this run)", (unsigned int)port);
        return;
    }
    int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    const int volume_result = sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    const int create_result = pthread_create(&thread, &attr, output_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (create_result != 0) {
        log_line("audio: output thread could not start (%d, no sound this run)", create_result);
        return;
    }
    pthread_detach(thread);
    log_line("audio: port %d open (48000 Hz stereo, %u frame blocks), volume result 0x%08X, output thread started", port,
        (unsigned int)BlockFrames, (unsigned int)volume_result);
}

void hm64vita::audio_queue_samples(const int16_t* samples, size_t sample_count) {
    const uint32_t frames = uint32_t(sample_count / 2);
    if (frames == 0) {
        return;
    }
    const uint32_t chunk = stat_chunks++;
    stat_input_frames += frames;
    if (chunk == 0) {
        log_line("audio: first chunk, %u frames at %u Hz", (unsigned int)frames, (unsigned int)input_rate.load());
    }

    if (port < 0 || buffered_frames() > MaxBufferedFrames) {
        stat_dropped_frames += frames;
        return;
    }

    // Linear resampling from the input rate to 48 kHz. position runs from the last frame of the previous chunk
    // (index -1) through this chunk.
    const float step = float(input_rate.load()) / float(OutputRate);
    uint32_t write = ring_write.load(std::memory_order_relaxed);
    const uint32_t space = RingFrames - buffered_frames();
    uint32_t written = 0;
    while (position < float(frames - 1) && written < space) {
        const int base = int(floorf(position));    // -1 .. frames - 2
        const float frac = position - float(base);
        float l0, r0;
        if (base < 0) {
            l0 = last_left;
            r0 = last_right;
        }
        else {
            // Channels arrive swapped.
            l0 = float(samples[base * 2 + 1]);
            r0 = float(samples[base * 2 + 0]);
        }
        const int next = base + 1;
        const float l1 = float(samples[next * 2 + 1]);
        const float r1 = float(samples[next * 2 + 0]);
        const float left = (l0 + (l1 - l0) * frac) * 0.5f;
        const float right = (r0 + (r1 - r0) * frac) * 0.5f;
        const uint32_t index = (write + written) % RingFrames;
        ring[index * 2 + 0] = int16_t(left);
        ring[index * 2 + 1] = int16_t(right);
        written++;
        position += step;
    }
    ring_write.store(write + written, std::memory_order_release);

    // Carry the last frame and the leftover position into the next chunk.
    last_left = float(samples[(frames - 1) * 2 + 1]);
    last_right = float(samples[(frames - 1) * 2 + 0]);
    position -= float(frames);
    if (position < -1.0f || position > 0.0f) {
        position = -1.0f;
    }
}

size_t hm64vita::audio_frames_remaining() {
    // In input-rate frames, as the runtime expects.
    return size_t(uint64_t(buffered_frames()) * input_rate.load() / OutputRate);
}

void hm64vita::audio_set_frequency(uint32_t freq) {
    log_line("audio: frequency set to %u", (unsigned int)freq);
    if (freq >= 4000 && freq <= 96000) {
        input_rate.store(freq);
    }
}
