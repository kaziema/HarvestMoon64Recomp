// RT64 render context for the Vita. Copied from RecompFrontend (recompui/src/renderer/rt64_render_context.cpp)
// with recompui, texture packs and the window/fullscreen handling removed. Every display list and screen update
// is timed, because RT64's front end runs inside these calls and its CPU cost is what Phase 0 measures.

#include "vita_rt64_context.h"

#include <cstring>
#include <memory>
#include <thread>

#include <psp2/kernel/processmgr.h>

#include "hle/rt64_application.h"
#include "gxm/rt64_gxm_renderer.h"
#include "vv/vv_core.h"

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#include "null_renderer.h"
#include "vita_timeline.h"
#include "vita_log.h"

namespace {
    uint8_t DMEM[0x1000];
    uint8_t IMEM[0x1000];
    uint8_t dummy_rom_header[0x40];

    unsigned int MI_INTR_REG = 0;
    unsigned int DPC_START_REG = 0;
    unsigned int DPC_END_REG = 0;
    unsigned int DPC_CURRENT_REG = 0;
    unsigned int DPC_STATUS_REG = 0;
    unsigned int DPC_CLOCK_REG = 0;
    unsigned int DPC_BUFBUSY_REG = 0;
    unsigned int DPC_PIPEBUSY_REG = 0;
    unsigned int DPC_TMEM_REG = 0;

    // Passed as RT64's window handle so it takes the "window supplied by the host" path and never creates one.
    // The Vita backend ignores it.
    int window_tag;

    void dummy_check_interrupts() {}

    uint32_t elapsed_since(uint64_t start_us) {
        return uint32_t(sceKernelGetProcessTimeWide() - start_us);
    }

    RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
        switch (option) {
            case ultramodern::renderer::AspectRatio::Original:
                return RT64::UserConfiguration::AspectRatio::Original;
            case ultramodern::renderer::AspectRatio::Expand:
                return RT64::UserConfiguration::AspectRatio::Expand;
            case ultramodern::renderer::AspectRatio::Manual:
                return RT64::UserConfiguration::AspectRatio::Manual;
            default:
                return RT64::UserConfiguration::AspectRatio::Original;
        }
    }

    // Native resolution, no MSAA, standard precision: the settings the Vita GXM backend will run with.
    void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
        application->userConfig.resolution = RT64::UserConfiguration::Resolution::Original;
        application->userConfig.aspectRatio = to_rt64(config.ar_option);
        application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        application->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
        application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original;
        application->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
        application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Double;
    }

    class VitaRT64Context final : public ultramodern::renderer::RendererContext {
    public:
        VitaRT64Context(uint8_t* rdram, bool developer_mode) {
            // VV core and the rt64-gxm renderer: RT64's recorded frames are drawn through these. They run on this
            // thread (the gfx thread), which is also the thread that processes display lists.
            vv::set_log_callback([](const char* message) { hm64vita::log_line("%s", message); });
            vv::CoreConfig vv_config;
            const bool vv_ready = vv::core_init(vv_config);
            hm64vita::log_line("VV core init: %s", vv_ready ? "ok" : "FAILED");
            hm64vita::log_memory("after VV core init");
            if (vv_ready) {
                const bool renderer_ready = rt64gxm::renderer_init([](const char* message) { hm64vita::log_line("%s", message); });
                hm64vita::log_line("rt64-gxm renderer init: %s", renderer_ready ? "ok" : "FAILED");
            }

            // RT64 sizes its shader compiler and texture cache thread pools from this.
            hm64vita::log_line("RT64: creating application (std::thread::hardware_concurrency %u)", std::thread::hardware_concurrency());
            hm64vita::log_memory("before RT64 setup");
            const uint64_t start_us = sceKernelGetProcessTimeWide();

            RT64::Application::Core appCore{};
            appCore.window = &window_tag;
            appCore.checkInterrupts = dummy_check_interrupts;

            appCore.HEADER = dummy_rom_header;
            appCore.RDRAM = rdram;
            appCore.DMEM = DMEM;
            appCore.IMEM = IMEM;

            appCore.MI_INTR_REG = &MI_INTR_REG;

            appCore.DPC_START_REG = &DPC_START_REG;
            appCore.DPC_END_REG = &DPC_END_REG;
            appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
            appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
            appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
            appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
            appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
            appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

            ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
            appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
            appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
            appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
            appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
            appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
            appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
            appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
            appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
            appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
            appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
            appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
            appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
            appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
            appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

            RT64::ApplicationConfiguration appConfig;
            appConfig.useConfigurationFile = false;

            app = std::make_unique<RT64::Application>(appCore, appConfig);

            set_application_user_config(app.get(), ultramodern::renderer::get_graphics_config());
            app->userConfig.developerMode = developer_mode;
            // Same enhancement settings as RecompFrontend.
            app->enhancementConfig.f3dex.forceBranch = true;
            app->enhancementConfig.textureLOD.scale = true;
            app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::Console;
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            // TEST 9 measured render to RAM at about 15 ms of a 45 ms frame. With a real GPU it would also mean
            // waiting for the GPU and reading every frame back, so the Vita runs without it. Effects where the game
            // reads its own framebuffer may need it; that can only be checked once frames are on screen.
            app->emulatorConfig.framebuffer.renderToRAM = false;

            const RT64::Application::SetupResult result = app->setup(0);
            hm64vita::log_line("RT64: setup returned %d in %u ms (0 = success; RT64's own messages are in ux0:data/hm64/stderr.log)", (int)result, elapsed_since(start_us) / 1000);
            hm64vita::log_memory("after RT64 setup");
            if (result != RT64::Application::SetupResult::Success) {
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                app = nullptr;
                return;
            }

            hm64vita::log_line("RT64: render to RAM %s, developer mode %s", app->emulatorConfig.framebuffer.renderToRAM ? "on" : "off",
                app->userConfig.developerMode ? "on" : "off");
            setup_result = ultramodern::renderer::SetupResult::Success;
            chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
        }

        ~VitaRT64Context() override = default;

        bool valid() override {
            return static_cast<bool>(app);
        }

        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override {
            return false;
        }

        void enable_instant_present() override {
            app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
            app->updateEnhancementConfig();
        }

        void send_dl(const OSTask* task) override {
            hm64vita::timeline_event(hm64vita::TimelineEvent::DisplayListBegin);
            const uint64_t start_us = sceKernelGetProcessTimeWide();
            app->state->rsp->reset();
            app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
            app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
            hm64vita::note_display_list(task, elapsed_since(start_us));
            hm64vita::timeline_event(hm64vita::TimelineEvent::DisplayListEnd);
        }

        void send_dummy_workload(uint32_t fb_address) override {
            app->state->listProcessBegin();
            app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
            app->state->rdp->setOtherMode(0x382C30, 0);
            app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
            app->state->fullSync();
            app->state->listProcessEnd();
        }

        void update_screen() override {
            hm64vita::timeline_event(hm64vita::TimelineEvent::ScreenUpdate);
            const uint64_t start_us = sceKernelGetProcessTimeWide();
            app->updateScreen();
            hm64vita::note_screen_update(elapsed_since(start_us));
        }

        void shutdown() override {
            if (app != nullptr) {
                app->end();
            }
        }

        uint32_t get_display_framerate() const override {
            return 60;
        }

        float get_resolution_scale() const override {
            return 1.0f;
        }

    private:
        std::unique_ptr<RT64::Application> app;
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> hm64vita::create_rt64_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<VitaRT64Context>(rdram, developer_mode);
}
