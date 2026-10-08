// High-level emulation (HLE) of Harvest Moon 64's audio microcode, n_aspMain.
//
// The recompiled microcode (rsp/n_aspMain.cpp) runs the RSP's vector code instruction by instruction on the CPU and
// costs about 22 ms per audio task on the Vita, which gates every frame. This replaces it with native code that does
// the same audio list commands directly: mupen64plus-rsp-hle's "naudio" implementation (src/vita/rsp_hle, GPL-2.0
// or later, compatible with this project's GPL-3.0). HM64's microcode data matches naudio: the word at +0x10 is
// 0x0000127C, and its command table (ADPCM, CLEARBUFF, ENVMIXER, ... with two no-op slots and the 0x02B0 command)
// is the same as rsp-hle's.
//
// The runtime's RDRAM and DMEM use the same layout rsp-hle expects (32-bit words in host order, byte addresses
// XOR 3), and it has already copied the OSTask to DMEM 0xFC0 and the microcode data to DMEM 0, so rsp-hle reads
// them in place.
//
// Verification: a few tasks (VerifyTasks) run both versions. The real microcode writes the real RDRAM; HLE runs on a
// private copy of RDRAM taken just before. Every byte HLE changed is then compared with what the real microcode
// wrote, and the result is logged. Outside those tasks only HLE runs.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <psp2/kernel/processmgr.h>

#include "librecomp/rsp.hpp"

#include "vita_log.h"

extern "C" {
#include "rsp_hle/hle_internal.h"
#include "rsp_hle/ucodes.h"
}

RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace {
    constexpr uint32_t RdramSize = 8 * 1024 * 1024;

    // Task numbers that run both versions and compare. Early ones cover the title screen, later ones gameplay.
    const uint32_t VerifyTasks[] = { 20, 21, 22, 300, 301, 1200, 1201, 3000 };

    hle_t hle;
    uint32_t task_count = 0;
    std::atomic<bool> use_hle{true};  // switchable at run time (vita_debug_controls.cpp)

    // Which audio command wrote which RDRAM range, for attributing verify mismatches. naudio commands that write
    // RDRAM: ADPCM, RESAMPLE and ENVMIXER save their state (32 to 80 bytes) at an address; SAVEBUFF writes the
    // audio itself; command 0x14 (filters) saves state too.
    const char* const CommandNames[16] = { "SPNOOP", "ADPCM", "CLEARBUFF", "ENVMIXER", "LOADBUFF", "RESAMPLE", "SAVEBUFF", "CMD7",
        "CMD8", "SETVOL", "DMEMMOVE", "LOADADPCM", "MIXER", "INTERLEAVE", "CMD14(02B0)", "SETLOOP" };
    struct Write {
        uint8_t command;
        uint32_t start;
        uint32_t end;
    };
    Write writes[512];
    uint32_t write_count = 0;
    uint32_t command_histogram[128];

    void scan_alist(const uint8_t* rdram) {
        write_count = 0;
        memset(command_histogram, 0, sizeof(command_histogram));
        const uint32_t data_ptr = *reinterpret_cast<const uint32_t*>(dmem + 0xFF0) & 0xFFFFFF;
        const uint32_t data_size = *reinterpret_cast<const uint32_t*>(dmem + 0xFF4);
        for (uint32_t i = 0; i + 8 <= data_size; i += 8) {
            const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + ((data_ptr + i) & 0xFFFFFF));
            const uint32_t w2 = *reinterpret_cast<const uint32_t*>(rdram + ((data_ptr + i + 4) & 0xFFFFFF));
            const uint8_t command = (w1 >> 24) & 0x7F;
            command_histogram[command]++;
            uint32_t start = 0, size = 0;
            switch (command) {
            case 1: start = w1 & 0xFFFFFF; size = 32; break;            // ADPCM state
            case 5: start = w1 & 0xFFFFFF; size = 32; break;            // RESAMPLE state
            case 3: start = w2 & 0xFFFFFF; size = 80; break;            // ENVMIXER state
            case 6: start = w2 & 0xFFFFFF; size = (w1 >> 12) & 0xFFF; break;  // SAVEBUFF
            default: break;
            }
            if (size > 0 && write_count < 512) {
                writes[write_count++] = { command, start, start + size };
            }
        }
    }

    // Buffer freshness (HM64 test 21, jumbled audio). NuSystem's audio manager hands the previous frame's buffer to
    // the audio interface at each retrace, assuming the audio task that renders it has finished, as it always has on
    // an N64. Here the task runs on the SP task thread, which can be held up (for example by the graphics thread at
    // the same priority), and then a stale or half-written buffer would be played. Every task records which RDRAM
    // ranges its SAVEBUFF commands wrote; when the game queues a buffer, audio_wait_for_buffer checks that a task has
    // written it since it was last queued, and waits for the task if not.
    struct WrittenRange {
        uint32_t start;
        uint32_t end;
        uint32_t task;
        uint64_t done_us;
    };
    constexpr uint32_t RangeSlots = 256;
    WrittenRange written_ranges[RangeSlots];
    std::atomic<uint32_t> written_count{0};        // ranges recorded, ever
    std::atomic<uint32_t> tasks_completed{0};
    std::atomic<uint32_t> tasks_started{0};
    uint8_t* rdram_base = nullptr;
    uint32_t pending_starts[64];
    uint32_t pending_ends[64];
    uint32_t pending_count = 0;

    void record_savebuff_ranges(const uint8_t* rdram) {
        pending_count = 0;
        const uint32_t data_ptr = *reinterpret_cast<const uint32_t*>(dmem + 0xFF0) & 0xFFFFFF;
        const uint32_t data_size = *reinterpret_cast<const uint32_t*>(dmem + 0xFF4);
        for (uint32_t i = 0; i + 8 <= data_size; i += 8) {
            const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + ((data_ptr + i) & 0xFFFFFF));
            const uint32_t w2 = *reinterpret_cast<const uint32_t*>(rdram + ((data_ptr + i + 4) & 0xFFFFFF));
            if (((w1 >> 24) & 0x7F) == 6 && pending_count < 64) {
                pending_starts[pending_count] = w2 & 0xFFFFFF;
                pending_ends[pending_count] = (w2 & 0xFFFFFF) + ((w1 >> 12) & 0xFFF);
                pending_count++;
            }
        }
    }

    void publish_savebuff_ranges(uint32_t task) {
        const uint64_t now = sceKernelGetProcessTimeWide();
        uint32_t count = written_count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < pending_count; i++) {
            written_ranges[count % RangeSlots] = { pending_starts[i], pending_ends[i], task, now };
            count++;
        }
        written_count.store(count, std::memory_order_release);
        tasks_completed.fetch_add(1, std::memory_order_release);
    }

    // Latest completed task that wrote any part of [start, end), or 0.
    uint32_t latest_writer(uint32_t start, uint32_t end, uint64_t* done_us) {
        const uint32_t count = written_count.load(std::memory_order_acquire);
        const uint32_t first = (count > RangeSlots) ? count - RangeSlots : 0;
        uint32_t best = 0;
        for (uint32_t i = first; i < count; i++) {
            const WrittenRange& r = written_ranges[i % RangeSlots];
            if (r.start < end && start < r.end && r.task >= best) {
                best = r.task;
                *done_us = r.done_us;
            }
        }
        return best;
    }

    // The task number each recently queued buffer address was last played with.
    struct Queued {
        uint32_t address;
        uint32_t task;
    };
    Queued queued[16];
    uint32_t queued_count = 0;

    std::atomic<uint32_t> stat_fresh{0};
    std::atomic<uint32_t> stat_waited{0};
    std::atomic<uint32_t> stat_wait_us_max{0};
    std::atomic<uint32_t> stat_wait_us_total{0};
    std::atomic<uint32_t> stat_stale{0};
    std::atomic<uint32_t> stat_unknown{0};
    std::atomic<uint32_t> stat_age_us_max{0};

    int command_for_address(uint32_t address) {
        for (uint32_t i = 0; i < write_count; i++) {
            if (address >= writes[i].start && address < writes[i].end) {
                return writes[i].command;
            }
        }
        return -1;
    }
    std::atomic<uint32_t> warning_count{0};

    uint8_t* before_copy = nullptr;  // RDRAM just before the task
    uint8_t* hle_copy = nullptr;     // the same, after HLE ran on it

    bool is_verify_task(uint32_t task) {
        for (uint32_t t : VerifyTasks) {
            if (t == task) {
                return true;
            }
        }
        return false;
    }

    bool last_verify_task(uint32_t task) {
        return task == VerifyTasks[sizeof(VerifyTasks) / sizeof(VerifyTasks[0]) - 1];
    }

    void run_hle(uint8_t* dram) {
        hle.dram = dram;
        hle.dmem = dmem;
        alist_process_naudio(&hle);
    }

    void verify(uint8_t* rdram, uint32_t ucode_addr, uint32_t task) {
        if (before_copy == nullptr) {
            before_copy = static_cast<uint8_t*>(malloc(RdramSize));
            hle_copy = static_cast<uint8_t*>(malloc(RdramSize));
            if (before_copy == nullptr || hle_copy == nullptr) {
                hm64vita::log_line("audio HLE verify: could not allocate 16 MB for the comparison, skipping verification");
                free(before_copy);
                free(hle_copy);
                before_copy = hle_copy = nullptr;
                run_hle(rdram);
                return;
            }
        }

        // DMEM holds the task and microcode data; both versions need the same starting DMEM.
        static uint8_t dmem_before[0x1000];
        memcpy(dmem_before, dmem, sizeof(dmem_before));
        memcpy(before_copy, rdram, RdramSize);
        memcpy(hle_copy, rdram, RdramSize);

        scan_alist(rdram);
        const uint64_t hle_start = sceKernelGetProcessTimeWide();
        run_hle(hle_copy);
        const uint32_t hle_us = uint32_t(sceKernelGetProcessTimeWide() - hle_start);

        memcpy(dmem, dmem_before, sizeof(dmem_before));
        const uint64_t lle_start = sceKernelGetProcessTimeWide();
        const RspExitReason lle_result = n_aspMain(rdram, ucode_addr);
        const uint32_t lle_us = uint32_t(sceKernelGetProcessTimeWide() - lle_start);

        // Compare every byte HLE changed with the real microcode's result.
        uint32_t hle_changed = 0;
        uint32_t mismatched = 0;
        uint32_t first_mismatch = 0xFFFFFFFF;
        uint32_t lowest = 0xFFFFFFFF;
        uint32_t highest = 0;
        int max_sample_diff = 0;
        // Mismatch kinds (HM64 test 19 showed many in game): rounding (every 16-bit half within 4), halves swapped
        // (a layout problem), or different. The first few are listed with both values.
        uint32_t rounding = 0;
        uint32_t swapped = 0;
        uint32_t different = 0;
        char examples[256] = "";
        size_t examples_len = 0;
        uint32_t examples_count = 0;
        uint32_t by_command[17] = {};  // index 16 = no known command wrote there
        int savebuff_max_diff = 0;
        for (uint32_t i = 0; i < RdramSize; i += 4) {
            const uint32_t before_word = *reinterpret_cast<const uint32_t*>(before_copy + i);
            const uint32_t hle_word = *reinterpret_cast<const uint32_t*>(hle_copy + i);
            if (before_word == hle_word) {
                continue;
            }
            hle_changed++;
            if (i < lowest) lowest = i;
            if (i > highest) highest = i;
            const uint32_t real_word = *reinterpret_cast<const uint32_t*>(rdram + i);
            if (real_word != hle_word) {
                mismatched++;
                if (first_mismatch == 0xFFFFFFFF) {
                    first_mismatch = i;
                }
                // Treat the word as two 16-bit samples to report how far off the audio is.
                int word_diff = 0;
                for (int h = 0; h < 2; h++) {
                    const int a = int16_t(real_word >> (16 * h));
                    const int b = int16_t(hle_word >> (16 * h));
                    const int d = (a > b) ? a - b : b - a;
                    if (d > max_sample_diff) max_sample_diff = d;
                    if (d > word_diff) word_diff = d;
                }
                const int command = command_for_address(i);
                by_command[(command >= 0 && command < 16) ? command : 16]++;
                if (command == 6 && word_diff > savebuff_max_diff) {
                    savebuff_max_diff = word_diff;
                }
                if (word_diff <= 4) {
                    rounding++;
                }
                else if (((real_word >> 16) | (real_word << 16)) == hle_word) {
                    swapped++;
                }
                else {
                    different++;
                    if (examples_count < 4 && examples_len < sizeof(examples) - 40) {
                        examples_count++;
                        examples_len += size_t(snprintf(examples + examples_len, sizeof(examples) - examples_len, " [0x%06X real %08X hle %08X]",
                            (unsigned int)i, (unsigned int)real_word, (unsigned int)hle_word));
                    }
                }
            }
        }
        hm64vita::log_line("audio HLE verify task %u: HLE changed %u words (0x%06X to 0x%06X), %u differ from the real microcode (first at 0x%06X, largest 16-bit difference %d) | HLE %u us, real microcode %u us, real exit %d",
            (unsigned int)task, (unsigned int)hle_changed, (unsigned int)(lowest == 0xFFFFFFFF ? 0 : lowest), (unsigned int)highest,
            (unsigned int)mismatched, (unsigned int)(first_mismatch == 0xFFFFFFFF ? 0 : first_mismatch), max_sample_diff,
            (unsigned int)hle_us, (unsigned int)lle_us, (int)lle_result);
        {
            char histogram[400] = "";
            size_t len = 0;
            for (int c = 0; c < 128 && len < sizeof(histogram) - 32; c++) {
                if (command_histogram[c] > 0) {
                    len += size_t(snprintf(histogram + len, sizeof(histogram) - len, " %s x%u", (c < 16) ? CommandNames[c] : "UNKNOWN",
                        (unsigned int)command_histogram[c]));
                }
            }
            hm64vita::log_line("audio HLE verify task %u: commands:%s", (unsigned int)task, histogram);
        }
        if (mismatched > 0) {
            char attribution[300] = "";
            size_t len = 0;
            for (int c = 0; c < 17 && len < sizeof(attribution) - 32; c++) {
                if (by_command[c] > 0) {
                    len += size_t(snprintf(attribution + len, sizeof(attribution) - len, " %s %u", (c < 16) ? CommandNames[c] : "unattributed",
                        (unsigned int)by_command[c]));
                }
            }
            hm64vita::log_line("audio HLE verify task %u: mismatched words by the command that wrote them:%s | largest difference in SAVEBUFF output (the audio heard): %d",
                (unsigned int)task, attribution, savebuff_max_diff);
            hm64vita::log_line("audio HLE verify task %u: mismatches are %u rounding (within 4), %u halves swapped, %u different;%s",
                (unsigned int)task, (unsigned int)rounding, (unsigned int)swapped, (unsigned int)different, examples);
        }

        if (last_verify_task(task)) {
            free(before_copy);
            free(hle_copy);
            before_copy = hle_copy = nullptr;
            hm64vita::log_line("audio HLE verify: done, comparison memory freed");
        }
    }
}

// rsp-hle's hooks. Only the message functions and rsp_break are reached by the audio list code.
extern "C" {
static void hle_message(const char* level, const char* format, va_list args) {
    const uint32_t count = warning_count++;
    if (count >= 20) {
        return;  // only the first few, the audio task runs constantly
    }
    char message[256];
    vsnprintf(message, sizeof(message), format, args);
    hm64vita::log_line("audio HLE %s: %s%s", level, message, (count == 19) ? " (further messages suppressed)" : "");
}

void HleVerboseMessage(void* user_defined, const char* message, ...) { (void)user_defined; (void)message; }
void HleInfoMessage(void* user_defined, const char* message, ...) { (void)user_defined; (void)message; }
void HleErrorMessage(void* user_defined, const char* message, ...) {
    (void)user_defined;
    va_list args;
    va_start(args, message);
    hle_message("error", message, args);
    va_end(args);
}
void HleWarnMessage(void* user_defined, const char* message, ...) {
    (void)user_defined;
    va_list args;
    va_start(args, message);
    hle_message("warning", message, args);
    va_end(args);
}

// Only the MP3 variant of naudio (Banjo-Tooie and others) calls this; HM64's naudio never does. Not compiled in.
void mp3_task(struct hle_t* hle_state, unsigned int index, uint32_t address) {
    (void)hle_state;
    (void)index;
    (void)address;
    hm64vita::log_line("audio HLE: unexpected MP3 command (index %u, address 0x%08X), ignored", index, (unsigned int)address);
}

// The runtime treats the task as finished when the microcode function returns; nothing to signal here.
void rsp_break(struct hle_t* hle_state, unsigned int setbits) {
    (void)hle_state;
    (void)setbits;
}
}

namespace hm64vita {
    RspExitReason audio_hle_task(uint8_t* rdram, uint32_t ucode_addr) {
        RspExitReason result = RspExitReason::Broke;
        const uint32_t task = task_count++;
        rdram_base = rdram;
        record_savebuff_ranges(rdram);
        tasks_started.fetch_add(1, std::memory_order_release);
        if (task == 0) {
            hm64vita::log_line("audio HLE: first audio task handled by rsp-hle naudio (verification against the original microcode %s)",
                hm64vita::diagnostics() ? "on tasks 20-22, 300-301, 1200-1201, 3000" : "off");
        }
        if (hm64vita::diagnostics() && is_verify_task(task)) {
            verify(rdram, ucode_addr, task);
        }
        else if (use_hle.load()) {
            run_hle(rdram);
        }
        else {
            result = n_aspMain(rdram, ucode_addr);
        }
        publish_savebuff_ranges(task + 1);
        return result;
    }

    void audio_wait_for_buffer(const void* samples, size_t bytes) {
        if (rdram_base == nullptr) {
            return;
        }
        const uint32_t start = uint32_t(static_cast<const uint8_t*>(samples) - rdram_base);
        const uint32_t end = start + uint32_t(bytes);
        uint32_t previous = 0;
        Queued* slot = nullptr;
        for (uint32_t i = 0; i < queued_count; i++) {
            if (queued[i].address == start) {
                slot = &queued[i];
                previous = slot->task;
            }
        }
        if (slot == nullptr) {
            slot = &queued[queued_count < 16 ? queued_count++ : (start / 16) % 16];
            slot->address = start;
            slot->task = 0;
        }

        uint64_t done_us = 0;
        uint32_t writer = latest_writer(start, end, &done_us);
        if (writer == 0 && previous == 0 && tasks_completed.load() == 0) {
            stat_unknown++;
            return;
        }
        if (writer > previous) {
            stat_fresh++;
        }
        else {
            // Not written since it was last played: wait for the audio task in flight (up to 40 ms).
            const uint64_t wait_start = sceKernelGetProcessTimeWide();
            while (writer <= previous && sceKernelGetProcessTimeWide() - wait_start < 40000) {
                sceKernelDelayThread(500);
                writer = latest_writer(start, end, &done_us);
            }
            const uint32_t waited = uint32_t(sceKernelGetProcessTimeWide() - wait_start);
            if (writer > previous) {
                stat_waited++;
                stat_wait_us_total += waited;
                if (waited > stat_wait_us_max.load()) stat_wait_us_max.store(waited);
            }
            else {
                stat_stale++;
            }
        }
        if (writer > 0) {
            const uint32_t age = uint32_t(sceKernelGetProcessTimeWide() - done_us);
            if (age > stat_age_us_max.load()) stat_age_us_max.store(age);
        }
        slot->task = writer;
    }

    uint8_t* rdram_pointer() {
        return rdram_base;
    }

    void audio_buffer_stats_log() {
        const uint32_t waited = stat_waited.load();
        hm64vita::log_line("audio buffers: %u fresh, %u waited for their task (avg %u us, max %u us), %u STALE (played without a new write), %u before any task | tasks started %u, completed %u | oldest buffer at play time %u us after its task",
            (unsigned int)stat_fresh.load(), (unsigned int)waited, (unsigned int)(waited ? stat_wait_us_total.load() / waited : 0),
            (unsigned int)stat_wait_us_max.load(), (unsigned int)stat_stale.load(), (unsigned int)stat_unknown.load(),
            (unsigned int)tasks_started.load(), (unsigned int)tasks_completed.load(), (unsigned int)stat_age_us_max.load());
        stat_wait_us_max.store(0);
        stat_age_us_max.store(0);
    }

    void audio_hle_set_enabled(bool enabled) {
        use_hle.store(enabled);
        hm64vita::log_line("audio HLE: %s (task %u)", enabled ? "ON, native audio code" : "OFF, original recompiled microcode", (unsigned int)task_count);
    }

    bool audio_hle_enabled() {
        return use_hle.load();
    }
}
