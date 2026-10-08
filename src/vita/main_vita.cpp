// PS Vita entry point for Harvest Moon 64: Recompiled (Phase 0: null renderer).
//
// The game runs with no graphics, audio or input. Display lists are counted and dropped, and a stats thread
// (vita_stats.cpp) writes the counts, game CPU busy % and per-thread CPU time to ux0:data/hm64/hm64.log,
// so the game's CPU speed on the Vita can be measured before any GXM work starts.
//
// Every startup stage logs a checkpoint, so a crash dump plus the last log line shows where it stopped.

#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <pthread.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#include "harvestmoon64_game.h"
#include "ovl_patches.hpp"
#include "null_renderer.h"
#if defined(HM64_VITA_RT64)
#include "vita_rt64_context.h"
#endif
#include "vita_audio.h"
#include "vita_debug_controls.h"
#include "vita_log.h"
#include "vita_selftest.h"
#include "vita_stats.h"
#include "vita_timeline.h"

// Default stack for every pthread / std::thread (read by VitaSDK's pthread_create).
// The VitaSDK default is PTHREAD_STACK_MIN (32 KB). The game threads run deeply nested recompiled
// N64 code, which is very likely to overflow 32 KB, so every thread gets 1 MB instead.
extern "C" { unsigned int _pthread_stack_default_user = 1 * 1024 * 1024; }

// newlib heap. The VitaSDK default is 128 MB. My other ports use 256 MB with ATTRIBUTE2=12 (set in vita/vita.cmake).
extern "C" { unsigned int _newlib_heap_size_user = 256 * 1024 * 1024; }

// Stack for the thread that runs startup and the recomp::start loop. The Vita main thread's stack size is set
// by the system, so the work moves to a thread with a known stack, the same as my other ports.
static const size_t game_main_stack_size = 2 * 1024 * 1024;

// The code segment can end close to a 64 KB boundary, and vita-elf-create needs a few KB of free space
// after it or the build fails with "segment 1 overlaps". This padding keeps the end of the segment clear of it.
extern "C" __attribute__((used)) const unsigned char hm64_text_segment_padding[32 * 1024] = { 0 };

// Data folder on the memory card. The ROM must be placed here as hm64.us.z64.
static const std::filesystem::path data_path = "ux0:data/hm64";
static const std::filesystem::path rom_path = data_path / "hm64.us.z64";

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

extern RspUcodeFunc n_aspMain;

// Times every audio microcode run, so the log shows its CPU cost and whether a run never returns.
namespace hm64vita {
    RspExitReason audio_hle_task(uint8_t* rdram, uint32_t ucode_addr);
}

static RspExitReason timed_n_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    hm64vita::timeline_event(hm64vita::TimelineEvent::AudioBegin);
    hm64vita::stats_audio_task_begin();
    // Native audio list processing (vita_audio_hle.cpp) in place of the recompiled microcode.
    RspExitReason result = hm64vita::audio_hle_task(rdram, ucode_addr);
    hm64vita::stats_audio_task_end();
    hm64vita::timeline_event(hm64vita::TimelineEvent::AudioEnd);
    return result;
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return timed_n_aspMain;
    default:
        // Hypothesis: the game sends an RSP task type with no microcode, and the runtime runs a null function.
        hm64vita::log_line("RSP: unknown task type %u, no microcode returned", (unsigned int)task->t.type);
        return nullptr;
    }
}

// Audio output: vita_audio.cpp.
static void queue_samples(int16_t* samples, size_t sample_count) {
    hm64vita::audio_queue_samples(samples, sample_count);
}
static size_t get_frames_remaining() { return hm64vita::audio_frames_remaining(); }
static void set_frequency(uint32_t freq) {
    hm64vita::audio_set_frequency(freq);
}

// Controller 1 from the Vita's buttons (layout chosen by the project's testers). What each N64 button does in
// Harvest Moon comes from the decomp (src/game/player.c):
//   Cross = A (interact, talk, lift, throw, confirm)      Circle = B (use tool, cancel, back)
//   Triangle = Start (main menu)                          Start = Z (inspect) in gameplay, Start everywhere else
//   L = L, R = R (rotate the camera)                      right stick = C buttons (rucksack, horse, dog, eat)
//   D-pad and left stick = stick (movement and menus). The game never reads the N64 D-pad, so the Vita D-pad
//   drives the stick at full tilt. Square and Select are unused.
static SceCtrlData ctrl_state;

namespace hm64vita {
    uint8_t* rdram_pointer();  // vita_audio_hle.cpp; null until the first audio task
}

// True while the game is in normal gameplay: mainLoopCallbackCurrentIndex (u16 at 0x8020564A, from the decomp's
// symbol list) is MAIN_GAME (1). Menus, the title and naming screens, dialogue and cutscenes use other values.
static bool in_gameplay() {
    const uint8_t* rdram = hm64vita::rdram_pointer();
    if (rdram == nullptr) {
        return false;
    }
    const uint32_t address = 0x0020564A;
    const uint16_t mode = *reinterpret_cast<const uint16_t*>(rdram + (address ^ 2));
    return mode == 1;
}
static std::atomic<uint32_t> ctrl_polls{0};
static std::atomic<uint32_t> ctrl_reads{0};
static uint32_t ctrl_logged_buttons = 0;

static float stick_axis(uint8_t value) {
    float v = (float(value) - 128.0f) / 127.0f;
    return (v < -1.0f) ? -1.0f : ((v > 1.0f) ? 1.0f : v);
}

static void poll_input() {
    SceCtrlData data;
    const int result = sceCtrlPeekBufferPositive(0, &data, 1);
    const uint32_t polls = ctrl_polls++;
    if (result < 0) {
        if (polls < 5) {
            hm64vita::log_line("input: sceCtrlPeekBufferPositive failed 0x%08X", (unsigned int)result);
        }
        return;
    }
    ctrl_state = data;
    if (polls == 0) {
        hm64vita::log_line("input: first poll, buttons 0x%08X, sticks %u,%u %u,%u", (unsigned int)data.buttons,
            (unsigned int)data.lx, (unsigned int)data.ly, (unsigned int)data.rx, (unsigned int)data.ry);
    }
    // Log each Vita button the first time it is pressed, so a test log shows input arriving.
    const uint32_t new_buttons = data.buttons & ~ctrl_logged_buttons;
    if (new_buttons != 0) {
        ctrl_logged_buttons |= new_buttons;
        hm64vita::log_line("input: first press of Vita buttons 0x%08X (poll %u, game reads so far %u)", (unsigned int)new_buttons,
            (unsigned int)polls, (unsigned int)ctrl_reads.load());
    }
}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
    if (controller_num != 0) {
        return false;
    }
    ctrl_reads++;
    const SceCtrlData data = ctrl_state;
    struct Mapping { uint32_t vita; uint16_t n64; };
    static const Mapping mappings[] = {
        { SCE_CTRL_CROSS, 0x8000 },     // A
        { SCE_CTRL_CIRCLE, 0x4000 },    // B
        { SCE_CTRL_TRIANGLE, 0x1000 },  // Start
        { SCE_CTRL_LTRIGGER, 0x0020 },  // L
        { SCE_CTRL_RTRIGGER, 0x0010 },  // R
    };
    uint16_t result = 0;
    for (const Mapping& m : mappings) {
        if (data.buttons & m.vita) {
            result |= m.n64;
        }
    }
    // Vita Start: Z (inspect the held item) in gameplay, Start in menus and on screens that say "push Start".
    if (data.buttons & SCE_CTRL_START) {
        result |= in_gameplay() ? 0x2000 : 0x1000;
    }

    // Right stick as C buttons.
    const float rx = stick_axis(data.rx);
    const float ry = stick_axis(data.ry);
    if (rx < -0.5f) result |= 0x0002;
    if (rx > 0.5f) result |= 0x0001;
    if (ry < -0.5f) result |= 0x0008;
    if (ry > 0.5f) result |= 0x0004;
    *buttons = result;

    // D-pad as the stick at full tilt; otherwise the left stick, with a small radial dead zone. The Vita's y axis
    // points down; the N64's points up.
    const float dx = ((data.buttons & SCE_CTRL_RIGHT) ? 1.0f : 0.0f) - ((data.buttons & SCE_CTRL_LEFT) ? 1.0f : 0.0f);
    const float dy = ((data.buttons & SCE_CTRL_UP) ? 1.0f : 0.0f) - ((data.buttons & SCE_CTRL_DOWN) ? 1.0f : 0.0f);
    if (dx != 0.0f || dy != 0.0f) {
        *x = dx;
        *y = dy;
        return true;
    }
    const float lx = stick_axis(data.lx);
    const float ly = -stick_axis(data.ly);
    if (lx * lx + ly * ly > 0.12f * 0.12f) {
        *x = lx;
        *y = ly;
    }
    return true;
}
static void set_rumble(int controller_num, bool rumble) {}
static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { .connected_device = ultramodern::input::Device::Controller, .connected_pak = ultramodern::input::Pak::None };
    }
    return { .connected_device = ultramodern::input::Device::None, .connected_pak = ultramodern::input::Pak::None };
}

static void message_box(const char* text) {
    hm64vita::log_line("MESSAGE BOX: %s", text);
}

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    hm64vita::log_line("checkpoint: create_gfx");
    return nullptr;
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    hm64vita::log_line("checkpoint: create_window");
    return {};
}

// Called by recomp::start roughly every millisecond. The stats thread logs how often it runs.
static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    static bool first_call = true;
    if (first_call) {
        first_call = false;
        hm64vita::log_line("checkpoint: main loop running (update_gfx first call)");
    }
    hm64vita::stats_note_main_loop();
}

// Logs every N64 OS thread as it starts on its own host thread (id and priority identify it), and tags the
// host thread in the stats thread table.
static std::string get_game_thread_name(const OSThread* t) {
    hm64vita::log_line("game thread starting: N64 thread id %d, priority %d", (int)t->id, (int)t->priority);
    hm64vita::stats_tag_n64_thread((int)t->id, (int)t->priority);
    return "[Game] " + std::to_string(t->id);
}

// Wraps the game's init callback so the log shows the game code was reached.
static void game_on_init_logged(uint8_t* rdram, recomp_context* ctx) {
    hm64vita::log_line("checkpoint: game on_init (rdram %p)", (void*)rdram);
    harvestmoon64::game_on_init(rdram, ctx);
    hm64vita::log_line("checkpoint: game on_init done, entering game entrypoint");
}

static std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0x68b2c3755c527305ULL,
        .internal_name = "Harvest Moon 64",
        .display_name = "Harvest Moon 64",
        .game_id = u8"harvest_moon_64",
        .mod_game_id = "hm64",
        .save_type = recomp::SaveType::AllowAll,
        .is_enabled = true,
        .decompression_routine = nullptr,
        .has_compressed_code = false,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = game_on_init_logged,
    },
};

// TEST 4 crash: start_game was called before recomp::start, so the VI thread's first tick already saw the game
// as started and skipped the dummy VI mode it sets while no game is running. The game had not called osViSetMode
// yet, so update_vi read a null VI mode. On desktop the launcher menu runs first, so the dummy mode is always set
// by then. Here the game is started from the VI callback after the first VI, which gives the same order.
static std::u8string game_to_start;
static std::atomic<bool> game_start_requested{false};

static void vi_callback() {
    hm64vita::timeline_event(hm64vita::TimelineEvent::VI);
    if (!game_start_requested.exchange(true)) {
        hm64vita::log_line("checkpoint: first VI done (dummy VI mode set), calling start_game");
        recomp::start_game(game_to_start, "");
        hm64vita::log_line("checkpoint: start_game called");
    }
}

static const char* rom_error_name(recomp::RomValidationError error) {
    switch (error) {
    case recomp::RomValidationError::Good: return "Good";
    case recomp::RomValidationError::FailedToOpen: return "FailedToOpen";
    case recomp::RomValidationError::NotARom: return "NotARom";
    case recomp::RomValidationError::IncorrectRom: return "IncorrectRom";
    case recomp::RomValidationError::NotYet: return "NotYet";
    case recomp::RomValidationError::IncorrectVersion: return "IncorrectVersion";
    case recomp::RomValidationError::OtherError: return "OtherError";
    default: return "Unknown";
    }
}

// Logs the calling thread's stack size, priority and core.
static void log_this_thread(const char* what) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const int result = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    if (result < 0) {
        hm64vita::log_line("%s: sceKernelGetThreadInfo failed 0x%08X", what, (unsigned int)result);
        return;
    }
    hm64vita::log_line("%s: '%s' stack %d KB, priority %d, cpu %d, affinity 0x%X", what, info.name,
        (int)(info.stackSize / 1024), (int)info.currentPriority, (int)info.currentCpuId, (unsigned int)info.currentCpuAffinityMask);
}

// Same clocks as my other ports. The Phase 0 CPU measurement is only valid at full ARM speed.
static void set_max_clocks() {
    hm64vita::log_clocks("before");
    const int arm = scePowerSetArmClockFrequency(444);
    const int bus = scePowerSetBusClockFrequency(222);
    const int gpu = scePowerSetGpuClockFrequency(222);
    const int xbar = scePowerSetGpuXbarClockFrequency(166);
    hm64vita::log_line("set clocks: arm 0x%08X, bus 0x%08X, gpu 0x%08X, gpu xbar 0x%08X", (unsigned int)arm,
        (unsigned int)bus, (unsigned int)gpu, (unsigned int)xbar);
    hm64vita::log_clocks("after");
}

static int game_main();

static void* game_main_thread(void*) {
    static int result;
    log_this_thread("game main thread");
    result = game_main();
    return &result;
}

int main(int argc, char** argv) {
    hm64vita::log_line("checkpoint: main() entered");

    // RT64 and the runtime report errors with printf/fprintf, which go nowhere on the Vita. Send them to files,
    // unbuffered so the last lines survive a crash.
    if (freopen("ux0:data/hm64/stdout.log", "w", stdout) != nullptr) {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
    if (freopen("ux0:data/hm64/stderr.log", "w", stderr) != nullptr) {
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    hm64vita::stats_register_thread(0, nullptr);
    log_this_thread("Vita main thread");
    hm64vita::log_memory("main");

    set_max_clocks();
    // Analog sampling, so the sticks report values (digital mode leaves them centred).
    const int ctrl_mode = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    hm64vita::log_line("input: sceCtrlSetSamplingMode returned 0x%08X", (unsigned int)ctrl_mode);
    hm64vita::audio_init();
    hm64vita::debug_controls_start();

    // The stats thread also runs the hang watchdog, so it starts before anything that could hang.
    hm64vita::stats_start();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, game_main_stack_size);
    pthread_t thread;
    const int create_result = pthread_create(&thread, &attr, game_main_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (create_result != 0) {
        hm64vita::log_line("game main thread could not start (%d), running on the Vita main thread instead", create_result);
        return game_main();
    }

    void* thread_result = nullptr;
    const int join_result = pthread_join(thread, &thread_result);
    hm64vita::log_line("game main thread finished (join %d)", join_result);
    return (join_result == 0 && thread_result != nullptr) ? *static_cast<int*>(thread_result) : EXIT_FAILURE;
}

static int game_main() {
    hm64vita::log_line("checkpoint: game_main() entered");
    hm64vita::run_threading_self_test();

    std::error_code ec;
    std::filesystem::create_directories(data_path, ec);
    if (ec) {
        hm64vita::log_line("create_directories(%s) failed: %s", data_path.string().c_str(), ec.message().c_str());
    }

    recomp::register_config_path(data_path);
    hm64vita::log_line("checkpoint: config path registered");

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }
    hm64vita::log_line("checkpoint: game registered");

    harvestmoon64::register_overlays();
    hm64vita::log_line("checkpoint: overlays registered");
    harvestmoon64::register_patches();
    hm64vita::log_line("checkpoint: patches registered");

    // Hypothesis: the ROM is missing, misnamed, or the wrong size. Check the file before the runtime does.
    std::error_code size_ec;
    const uintmax_t rom_size = std::filesystem::file_size(rom_path, size_ec);
    if (size_ec) {
        hm64vita::log_line("ROM file %s: cannot read size (%s)", rom_path.string().c_str(), size_ec.message().c_str());
    }
    else {
        hm64vita::log_line("ROM file %s: %llu bytes (expected 16777216)", rom_path.string().c_str(), (unsigned long long)rom_size);
    }

    // select_rom takes the game ID as input: it validates the ROM against that game's entry. TEST 2 passed an
    // empty ID, so the game lookup failed and it returned OtherError without opening the file.
    std::u8string game_id = supported_games[0].game_id;
    const uint64_t select_start_us = sceKernelGetProcessTimeWide();
    recomp::RomValidationError rom_error = recomp::select_rom(rom_path, game_id);
    hm64vita::log_line("select_rom: %s (%u ms)", rom_error_name(rom_error),
        (unsigned int)((sceKernelGetProcessTimeWide() - select_start_us) / 1000));
    if (rom_error != recomp::RomValidationError::Good) {
        hm64vita::log_line("ROM not valid. Place the US ROM at ux0:data/hm64/hm64.us.z64");
        return EXIT_FAILURE;
    }

    // select_rom copies the ROM to the config folder and ignores whether the write worked. recomp::start loads
    // that copy and shows "Error opening stored ROM" if it is missing or damaged, so check the copy here.
    const std::filesystem::path stored_rom_path = data_path / supported_games[0].stored_filename();
    std::error_code stored_ec;
    const uintmax_t stored_size = std::filesystem::file_size(stored_rom_path, stored_ec);
    if (stored_ec) {
        hm64vita::log_line("stored ROM %s: cannot read size (%s)", stored_rom_path.string().c_str(), stored_ec.message().c_str());
    }
    else {
        hm64vita::log_line("stored ROM %s: %llu bytes (expected 16777216)", stored_rom_path.string().c_str(), (unsigned long long)stored_size);
    }

    // There is no launcher menu, so the game starts immediately in the default game mode.
    // The game itself is started from vi_callback after the first VI (see the TEST 4 note above vi_callback).
    game_to_start = game_id;

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    // Phase 0 step 2 runs RT64's front end on a null GPU backend (HM64_VITA_RT64, the default). Turning it off in
    // vita/vita.cmake goes back to the null renderer that drops display lists, for comparison.
    ultramodern::renderer::callbacks_t renderer_callbacks{
#if defined(HM64_VITA_RT64)
        .create_render_context = hm64vita::create_rt64_render_context,
#else
        .create_render_context = hm64vita::create_null_render_context,
#endif
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = vi_callback,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = get_game_thread_name,
    };

    hm64vita::log_line("checkpoint: calling recomp::start");
    try {
        recomp::start(
            recomp::Version{},
            {},
            rsp_callbacks,
            renderer_callbacks,
            audio_callbacks,
            input_callbacks,
            gfx_callbacks,
            events_callbacks,
            error_handling_callbacks,
            threads_callbacks
        );
    }
    catch (const std::exception& e) {
        hm64vita::log_line("recomp::start threw: %s", e.what());
        hm64vita::log_memory("recomp::start threw");
        return EXIT_FAILURE;
    }

    hm64vita::log_line("checkpoint: recomp::start returned, exiting");
    return EXIT_SUCCESS;
}
