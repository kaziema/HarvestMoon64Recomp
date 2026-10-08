// Audio output for the Vita. See vita_audio.h.
//
// The game's audio thread hands over interleaved 16-bit stereo at the game's rate (32 kHz for Harvest Moon) with
// left and right swapped (the runtime's address xor). Samples are swapped back, halved in volume like the PC
// version, resampled linearly to 48 kHz and written to a ring buffer. A dedicated thread drains the ring into the
// main audio port (48 kHz only) in fixed blocks; sceAudioOutOutput blocks until the hardware needs more, so the
// thread runs at the hardware's pace.
// HM64 test 19: with only about 20 ms buffered, partial blocks and frequent short gaps made the sound choppy. Now a
// block is only played when a whole one is buffered; after running dry, playback waits for PrefillFrames before it
// resumes (one longer gap instead of many short ones). The buffered amount reported to the game is reduced by
// TargetFrames, so the game's audio code (which sizes each audio frame from what is still buffered) makes extra
// audio until about that much is queued, as the PC version does with its one-frame offset.

#include "vita_audio.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <psp2/audioout.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "vita_log.h"

namespace hm64vita {
    void audio_wait_for_buffer(const void* samples, size_t bytes);
    void audio_buffer_stats_log();
}

namespace {
    constexpr uint32_t OutputRate = 48000;
    constexpr uint32_t BlockFrames = 768;           // 16 ms per hardware block
    constexpr uint32_t RingFrames = 48000 / 2;      // 500 ms of 48 kHz stereo
    constexpr uint32_t MaxBufferedFrames = 48000 / 5; // above 200 ms buffered, incoming audio is dropped
    constexpr uint32_t PrefillFrames = BlockFrames * 3;   // 48 ms queued before playback resumes after running dry
    constexpr uint32_t TargetFrames = BlockFrames * 3;    // hidden from the game, so it keeps about this much queued

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
    uint32_t stat_rebuffers = 0;
    uint32_t stat_output_errors = 0;
    int last_output_error = 0;

    // Diagnostics (HM64 test 20, garbled audio).
    // Buffering mode, switchable at run time: 1 = test 20 (whole blocks, refill, 48 ms hidden from the game),
    // 0 = test 19 (partial blocks, no refill, nothing hidden).
    std::atomic<int> buffering_mode{1};

    // Recordings: the last DumpSeconds of what the game handed over (input rate, channels swapped back to L, R) and
    // of what went to the audio port (48 kHz). Saved to ux0:data/hm64 on request.
    constexpr uint32_t DumpSeconds = 20;
    constexpr uint32_t InputDumpFrames = 32000 * DumpSeconds;
    constexpr uint32_t OutputDumpFrames = 48000 * DumpSeconds;
    int16_t* input_dump = nullptr;
    int16_t* output_dump = nullptr;
    std::atomic<uint32_t> input_dump_pos{0};   // frames written, ever
    std::atomic<uint32_t> output_dump_pos{0};

    void dump_frames(int16_t* dump, uint32_t capacity, std::atomic<uint32_t>& pos, const int16_t* frames, uint32_t count) {
        if (dump == nullptr) {
            return;
        }
        uint32_t p = pos.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t index = (p + i) % capacity;
            dump[index * 2 + 0] = frames[i * 2 + 0];
            dump[index * 2 + 1] = frames[i * 2 + 1];
        }
        pos.store(p + count, std::memory_order_release);
    }

    bool save_dump(const char* path, const int16_t* dump, uint32_t capacity, uint32_t pos) {
        FILE* file = fopen(path, "wb");
        if (file == nullptr) {
            return false;
        }
        // Oldest first. Copied to a snapshot before writing: the file write is slow and the ring keeps filling,
        // which corrupted the second half of the test 21 recordings.
        const uint32_t count = (pos < capacity) ? pos : capacity;
        const uint32_t start = (pos < capacity) ? 0 : pos % capacity;
        int16_t* snapshot = static_cast<int16_t*>(malloc(size_t(count) * 2 * sizeof(int16_t)));
        if (snapshot == nullptr) {
            fclose(file);
            return false;
        }
        const uint32_t first_part = (capacity - start < count) ? capacity - start : count;
        memcpy(snapshot, &dump[size_t(start) * 2], size_t(first_part) * 2 * sizeof(int16_t));
        memcpy(snapshot + size_t(first_part) * 2, dump, size_t(count - first_part) * 2 * sizeof(int16_t));
        fwrite(snapshot, sizeof(int16_t) * 2, count, file);
        free(snapshot);
        fclose(file);
        return true;
    }

    uint32_t buffered_frames() {
        return ring_write.load(std::memory_order_acquire) - ring_read.load(std::memory_order_acquire);
    }

    // Logged from another thread (audio_log_stats): a log write goes to the memory card, and doing it here stalled
    // this thread long enough to run the hardware dry every 10 seconds (HM64 test 22).
    std::atomic<uint32_t> stat_log_requests{0};

    void* output_thread(void*) {
        sceKernelChangeThreadPriority(0, 65);
        // sceAudioOutOutput returns while the hardware is still playing the block it was given, so the next block
        // must go in a different buffer. With one buffer (tests 17 to 22) each block was overwritten while it played:
        // crackle and stutter on the speaker, while the recordings, taken before the hand-off, were clean.
        static int16_t blocks[3][BlockFrames * 2];
        uint32_t block_index = 0;
        bool playing = false;
        while (true) {
            int16_t* block = blocks[block_index];
            block_index = (block_index + 1) % 3;
            const uint32_t available = buffered_frames();
            uint32_t take;
            if (buffering_mode.load() == 1) {
                if (!playing && available >= PrefillFrames) {
                    playing = true;
                }
                else if (playing && available < BlockFrames) {
                    playing = false;
                    stat_rebuffers++;
                }
                take = playing ? BlockFrames : 0;
            }
            else {
                take = (available < BlockFrames) ? available : BlockFrames;
            }
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

            dump_frames(output_dump, OutputDumpFrames, output_dump_pos, block, BlockFrames);
            const int result = sceAudioOutOutput(port, block);
            stat_output_blocks++;
            if (result < 0) {
                stat_output_errors++;
                last_output_error = result;
                sceKernelDelayThread(16000);
            }

        }
        return nullptr;
    }

    void log_stats_now() {
        hm64vita::log_line("audio: input %u Hz, %u chunks, %u input frames, %u dropped (over 200 ms buffered) | output %u blocks, %u silent blocks (%u silent frames, %u times ran dry), %u errors (last 0x%08X), %u ms buffered",
            (unsigned int)input_rate.load(), (unsigned int)stat_chunks.load(), (unsigned int)stat_input_frames.load(),
            (unsigned int)stat_dropped_frames.load(), (unsigned int)stat_output_blocks, (unsigned int)stat_underrun_blocks,
            (unsigned int)stat_underrun_frames, (unsigned int)stat_rebuffers, (unsigned int)stat_output_errors, (unsigned int)last_output_error,
            (unsigned int)(buffered_frames() * 1000 / OutputRate));
        hm64vita::audio_buffer_stats_log();
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

    if (hm64vita::diagnostics()) {
        input_dump = static_cast<int16_t*>(malloc(size_t(InputDumpFrames) * 2 * sizeof(int16_t)));
        output_dump = static_cast<int16_t*>(malloc(size_t(OutputDumpFrames) * 2 * sizeof(int16_t)));
    }
    log_line("audio: recording buffers %s (%u s each side)", (input_dump && output_dump) ? "ready" : "NOT available", (unsigned int)DumpSeconds);

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
    // Make sure the audio task that renders this buffer has finished (see vita_audio_hle.cpp).
    hm64vita::audio_wait_for_buffer(samples, sample_count * sizeof(int16_t));
    const uint32_t chunk = stat_chunks++;
    stat_input_frames += frames;
    if (chunk == 0) {
        log_line("audio: first chunk, %u frames at %u Hz", (unsigned int)frames, (unsigned int)input_rate.load());
    }

    // Record the chunk as the game made it, channels put back in L, R order.
    if (input_dump != nullptr) {
        static int16_t swapped[4096 * 2];
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t n = (frames - done < 4096) ? frames - done : 4096;
            for (uint32_t i = 0; i < n; i++) {
                swapped[i * 2 + 0] = samples[(done + i) * 2 + 1];
                swapped[i * 2 + 1] = samples[(done + i) * 2 + 0];
            }
            dump_frames(input_dump, InputDumpFrames, input_dump_pos, swapped, n);
            done += n;
        }
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
    // In input-rate frames, as the runtime expects, less the cushion the game should keep filled (mode 1).
    const uint32_t buffered = buffered_frames();
    const uint32_t target = (buffering_mode.load() == 1) ? TargetFrames : 0;
    const uint32_t reported = (buffered > target) ? buffered - target : 0;
    return size_t(uint64_t(reported) * input_rate.load() / OutputRate);
}

void hm64vita::audio_set_frequency(uint32_t freq) {
    log_line("audio: frequency set to %u", (unsigned int)freq);
    if (freq >= 4000 && freq <= 96000) {
        input_rate.store(freq);
    }
}

void hm64vita::audio_set_buffering_mode(int mode) {
    buffering_mode.store(mode);
    log_line("audio: buffering mode %d (%s), at input recording frame %u, output recording frame %u", mode,
        mode == 1 ? "test 20: whole blocks, refill, 48 ms hidden" : "test 19: partial blocks, nothing hidden",
        (unsigned int)input_dump_pos.load(), (unsigned int)output_dump_pos.load());
}

int hm64vita::audio_buffering_mode() {
    return buffering_mode.load();
}

void hm64vita::audio_save_recordings(const char* tag) {
    char path[128];
    const uint32_t in_pos = input_dump_pos.load();
    const uint32_t out_pos = output_dump_pos.load();
    snprintf(path, sizeof(path), "ux0:data/hm64/audio_%s_game_32k.raw", tag);
    const bool in_ok = save_dump(path, input_dump, InputDumpFrames, in_pos);
    snprintf(path, sizeof(path), "ux0:data/hm64/audio_%s_speaker_48k.raw", tag);
    const bool out_ok = save_dump(path, output_dump, OutputDumpFrames, out_pos);
    log_line("audio: recordings '%s' saved (game side %s, %u frames recorded so far; speaker side %s, %u frames so far). Files: audio_%s_game_32k.raw and audio_%s_speaker_48k.raw, 16-bit stereo",
        tag, in_ok ? "ok" : "FAILED", (unsigned int)in_pos, out_ok ? "ok" : "FAILED", (unsigned int)out_pos, tag, tag);
}

void hm64vita::audio_log_stats() {
    // Called every 10 seconds from the debug controls thread: the first few minutes, then every minute.
    if (!hm64vita::diagnostics()) {
        return;
    }
    const uint32_t index = ++stat_log_requests;
    if (index <= 18 || (index % 6) == 0) {
        log_stats_now();
    }
}
