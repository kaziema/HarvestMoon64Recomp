// PS Vita entry point for Harvest Moon 64: Recompiled (Phase 0: null renderer).
//
// The game runs with no graphics, audio or input. Display lists are counted and dropped,
// and the counts are written to a log once per second so the game's CPU speed on the Vita
// can be measured before any GXM work starts.

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "recomp.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#include "harvestmoon64_game.h"
#include "ovl_patches.hpp"
#include "null_renderer.h"

// Data folder on the memory card. The ROM must be placed here as hm64.us.z64.
static const std::filesystem::path data_path = "ux0:data/hm64";
static const std::filesystem::path rom_path = data_path / "hm64.us.z64";
static const std::filesystem::path log_path = data_path / "phase0.log";

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

extern RspUcodeFunc n_aspMain;

static FILE* log_file = nullptr;

static void log_line(const char* text) {
    if (log_file != nullptr) {
        fputs(text, log_file);
        fputc('\n', log_file);
        fflush(log_file);
    }
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return n_aspMain;
    default:
        return nullptr;
    }
}

// Audio is discarded in Phase 0. Reporting an empty buffer keeps the game producing audio
// so the audio microcode's CPU cost is still included in the measurement.
static void queue_samples(int16_t* samples, size_t sample_count) {}
static size_t get_frames_remaining() { return 0; }
static void set_frequency(uint32_t freq) {}

static void poll_input() {}
static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
    return controller_num == 0;
}
static void set_rumble(int controller_num, bool rumble) {}
static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { .connected_device = ultramodern::input::Device::Controller, .connected_pak = ultramodern::input::Pak::None };
    }
    return { .connected_device = ultramodern::input::Device::None, .connected_pak = ultramodern::input::Pak::None };
}

static void message_box(const char* text) {
    log_line(text);
}

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    return nullptr;
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    return {};
}

// Called by recomp::start roughly every millisecond. Logs one line per second.
static void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last_log = clock::now();
    const clock::time_point now = clock::now();
    if (now - last_log >= std::chrono::seconds(1)) {
        last_log = now;
        char line[128];
        snprintf(line, sizeof(line), "display lists %" PRIu32 ", screen updates %" PRIu32,
            hm64vita::get_display_list_count(), hm64vita::get_screen_update_count());
        log_line(line);
    }
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
        .on_init_callback = harvestmoon64::game_on_init,
    },
};

int main(int argc, char** argv) {
    std::error_code ec;
    std::filesystem::create_directories(data_path, ec);
    log_file = fopen(log_path.string().c_str(), "w");
    log_line("Harvest Moon 64 Vita, Phase 0 (null renderer)");

    recomp::register_config_path(data_path);

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    harvestmoon64::register_overlays();
    harvestmoon64::register_patches();

    std::u8string game_id;
    recomp::RomValidationError rom_error = recomp::select_rom(rom_path, game_id);
    if (rom_error != recomp::RomValidationError::Good) {
        log_line("ROM not found or not valid. Place the US ROM at ux0:data/hm64/hm64.us.z64");
        if (log_file != nullptr) {
            fclose(log_file);
        }
        return EXIT_FAILURE;
    }

    // There is no launcher menu, so the game starts immediately in the default game mode.
    recomp::start_game(game_id, "");

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = hm64vita::create_null_render_context,
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
        .vi_callback = nullptr,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{};

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

    if (log_file != nullptr) {
        fclose(log_file);
    }

    return EXIT_SUCCESS;
}
