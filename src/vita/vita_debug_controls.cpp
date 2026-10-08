// Diagnostic controls on the front touch screen (the game does not use touch). Hold a corner for one second:
//   top left:     audio code on/off (native HLE audio vs the original recompiled microcode)
//   top right:    audio buffering mode (test 20 behaviour vs test 19 behaviour)
//   bottom left:  save the last 20 seconds of audio to ux0:data/hm64 (game side and speaker side)
//   bottom right: capture the next frame (screen image, every draw call, their textures) to ux0:data/hm64
// With diagnostics on (ux0:data/hm64/diagnostics.txt), recordings are also saved automatically at 90 and 180 seconds
// and frames captured at 60, 120 and 180 seconds; audio recordings are only kept with diagnostics on.
// Every action is logged with its time.

#include "vita_debug_controls.h"

#include <cstdio>

#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include "gxm/rt64_gxm_renderer.h"
#include "vita_audio.h"
#include "vita_log.h"

namespace hm64vita {
    void audio_hle_set_enabled(bool enabled);
    bool audio_hle_enabled();
}

namespace {
    enum Corner { None, TopLeft, TopRight, BottomLeft, BottomRight };

    // Front panel coordinates are 0..1919 by 0..1087.
    Corner corner_of(int x, int y) {
        const bool left = x < 480;
        const bool right = x > 1440;
        const bool top = y < 272;
        const bool bottom = y > 816;
        if (top && left) return TopLeft;
        if (top && right) return TopRight;
        if (bottom && left) return BottomLeft;
        if (bottom && right) return BottomRight;
        return None;
    }

    void* controls_thread(void*) {
        sceKernelChangeThreadPriority(0, 160);
        Corner held = None;
        uint32_t held_ms = 0;
        bool fired = false;
        uint32_t elapsed_ms = 0;
        uint32_t manual_saves = 0;
        uint32_t manual_captures = 0;
        uint32_t auto_captures = 0;
        bool auto1 = false;
        bool auto2 = false;
        while (true) {
            sceKernelDelayThread(100 * 1000);
            elapsed_ms += 100;

            SceTouchData touch;
            Corner now = None;
            if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0) {
                now = corner_of(touch.report[0].x, touch.report[0].y);
            }
            if (now != held) {
                held = now;
                held_ms = 0;
                fired = false;
            }
            else if (held != None && !fired) {
                held_ms += 100;
                if (held_ms >= 1000) {
                    fired = true;
                    switch (held) {
                    case TopLeft:
                        hm64vita::audio_hle_set_enabled(!hm64vita::audio_hle_enabled());
                        break;
                    case TopRight:
                        hm64vita::audio_set_buffering_mode(hm64vita::audio_buffering_mode() == 1 ? 0 : 1);
                        break;
                    case BottomLeft: {
                        char tag[32];
                        snprintf(tag, sizeof(tag), "manual%u", (unsigned int)++manual_saves);
                        hm64vita::audio_save_recordings(tag);
                        break;
                    }
                    case BottomRight: {
                        char prefix[64];
                        snprintf(prefix, sizeof(prefix), "ux0:data/hm64/capture_manual%u", (unsigned int)++manual_captures);
                        rt64gxm::request_capture(prefix);
                        hm64vita::log_line("debug controls: frame capture requested (%s)", prefix);
                        break;
                    }
                    default:
                        break;
                    }
                }
            }

            if (elapsed_ms % 10000 == 0) {
                hm64vita::audio_log_stats();
            }
            const bool automatic = hm64vita::diagnostics();
            if (automatic && auto_captures < 3 && elapsed_ms >= (auto_captures + 1) * 60 * 1000) {
                auto_captures++;
                char prefix[64];
                snprintf(prefix, sizeof(prefix), "ux0:data/hm64/capture_auto%us", (unsigned int)(auto_captures * 60));
                rt64gxm::request_capture(prefix);
                hm64vita::log_line("debug controls: automatic frame capture requested (%s)", prefix);
            }
            if (automatic && !auto1 && elapsed_ms >= 90 * 1000) {
                auto1 = true;
                hm64vita::audio_save_recordings("auto90s");
            }
            if (automatic && !auto2 && elapsed_ms >= 180 * 1000) {
                auto2 = true;
                hm64vita::audio_save_recordings("auto180s");
            }
        }
        return nullptr;
    }
}

void hm64vita::debug_controls_start() {
    const int sampling = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 32 * 1024);
    const int result = pthread_create(&thread, &attr, controls_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (result == 0) {
        pthread_detach(thread);
    }
    log_line("debug controls: touch sampling 0x%08X, thread %s. Hold a corner 1 s: top left = audio code on/off, top right = buffering mode, bottom left = save audio recordings, bottom right = capture frame",
        (unsigned int)sampling, result == 0 ? "started" : "FAILED");
}
