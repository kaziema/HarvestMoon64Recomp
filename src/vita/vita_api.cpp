// PS Vita versions of the functions the game patches import (upstream: src/game/recomp_api.cpp).
//
// Upstream implements these on top of recompui (menus) and recompinput (SDL input), which the
// Vita build does not include yet. Phase 0 values: a fixed 960x544 screen, the original aspect
// ratio, full volume and no input.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/helpers.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/error_handling.hpp"
#include "../../lib/N64ModernRuntime/thirdparty/xxHash/xxh3.h"

static constexpr int VitaScreenWidth = 960;
static constexpr int VitaScreenHeight = 544;

extern "C" void recomp_update_inputs(uint8_t* rdram, recomp_context* ctx) {}

extern "C" void recomp_puts(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) cur_str = _arg<0, PTR(char)>(rdram, ctx);
    u32 length = _arg<1, u32>(rdram, ctx);

    for (u32 i = 0; i < length; i++) {
        fputc(MEM_B(i, (gpr)cur_str), stdout);
    }
}

extern "C" void recomp_exit(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::quit();
}

extern "C" void recomp_error(uint8_t* rdram, recomp_context* ctx) {
    std::string str{};
    PTR(u8) str_ptr = _arg<0, PTR(u8)>(rdram, ctx);

    for (size_t i = 0; MEM_B(str_ptr, i) != '\x00'; i++) {
        str += (char)MEM_B(str_ptr, i);
    }

    ultramodern::error_handling::message_box(str.c_str());
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

extern "C" void recomp_get_gyro_deltas(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    *x_out = 0.0f;
    *y_out = 0.0f;
}

extern "C" void recomp_get_mouse_deltas(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    *x_out = 0.0f;
    *y_out = 0.0f;
}

extern "C" void recomp_powf(uint8_t* rdram, recomp_context* ctx) {
    float a = _arg<0, float>(rdram, ctx);
    float b = ctx->f14.fl;

    _return(ctx, std::pow(a, b));
}

extern "C" void recomp_get_target_framerate(uint8_t* rdram, recomp_context* ctx) {
    int frame_divisor = _arg<0, u32>(rdram, ctx);

    _return(ctx, ultramodern::get_target_framerate(60 / frame_divisor));
}

extern "C" void recomp_get_window_resolution(uint8_t* rdram, recomp_context* ctx) {
    gpr width_out = _arg<0, PTR(u32)>(rdram, ctx);
    gpr height_out = _arg<1, PTR(u32)>(rdram, ctx);

    MEM_W(0, width_out) = (u32)VitaScreenWidth;
    MEM_W(0, height_out) = (u32)VitaScreenHeight;
}

extern "C" void recomp_get_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    float original = _arg<0, float>(rdram, ctx);

    _return(ctx, original);
}

extern "C" void recomp_time_us(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(ultramodern::time_since_start()).count()));
}

extern "C" void recomp_load_overlays(uint8_t* rdram, recomp_context* ctx) {
    u32 rom = _arg<0, u32>(rdram, ctx);
    PTR(void) ram = _arg<1, PTR(void)>(rdram, ctx);
    u32 size = _arg<2, u32>(rdram, ctx);

    load_overlays(rom, ram, size);
}

extern "C" void recomp_high_precision_fb_enabled(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(0));
}

extern "C" void recomp_get_resolution_scale(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, ultramodern::get_resolution_scale());
}

extern "C" void recomp_get_music_volume(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, 1.0f);
}

extern "C" void recomp_get_ambience_volume(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, 1.0f);
}

extern "C" void recomp_get_right_analog_inputs(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    *x_out = 0.0f;
    *y_out = 0.0f;
}

extern "C" void recomp_set_right_analog_suppressed(uint8_t* rdram, recomp_context* ctx) {}

// There is no recompui on the Vita, so there are never any queued UI callbacks to run.
extern "C" void recomp_run_ui_callbacks(uint8_t* rdram, recomp_context* ctx) {}

constexpr uint32_t k1_to_phys(uint32_t addr) {
    return addr & 0x1FFFFFFF;
}

extern "C" void osPiReadIo_recomp(RDRAM_ARG recomp_context* ctx) {
    uint32_t devAddr = recomp::rom_base | ctx->r4;
    gpr dramAddr = ctx->r5;
    uint32_t physical_addr = k1_to_phys(devAddr);

    if (physical_addr > recomp::rom_base) {
        // cart rom
        recomp::do_rom_pio(PASS_RDRAM dramAddr, physical_addr);
    }
    else {
        // sram
        assert(false && "SRAM ReadIo unimplemented");
    }

    ctx->r2 = 0;
}

extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 11; // PFS_ERR_DEVICE
}

extern "C" void recomp_abort(uint8_t* rdram, recomp_context* ctx) {
    std::string msg = _arg_string<0>(rdram, ctx);
    ultramodern::error_handling::message_box(msg.c_str());
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

extern "C" void recomp_xxh3(uint8_t* rdram, recomp_context* ctx) {
    PTR(void) data = _arg<0, PTR(void)>(rdram, ctx);
    u32 size = _arg<1, u32>(rdram, ctx);
    XXH3_state_t xxh3;
    XXH3_64bits_reset(&xxh3);

    // Hash 1 byte at a time to account for byteswapping.
    for (size_t i = 0; i < size; i++) {
        XXH3_64bits_update(&xxh3, TO_PTR(u8, data + i), 1);
    }

    uint64_t ret = XXH3_64bits_digest(&xxh3);

    ctx->r2 = (int32_t)(ret >> 32);
    ctx->r3 = (int32_t)(ret >> 0);
}
