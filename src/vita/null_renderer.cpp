#include "null_renderer.h"
#include "vita_log.h"

#include <atomic>

namespace hm64vita {
    static std::atomic<uint32_t> display_list_count = 0;
    static std::atomic<uint32_t> screen_update_count = 0;
    static std::atomic<uint64_t> display_list_us = 0;
    static std::atomic<uint32_t> display_list_max_us = 0;
    static std::atomic<uint64_t> screen_update_us = 0;
    static std::atomic<uint32_t> screen_update_max_us = 0;

    static void raise_max(std::atomic<uint32_t>& max, uint32_t value) {
        uint32_t current = max.load();
        while (value > current && !max.compare_exchange_weak(current, value)) {
        }
    }

    void note_display_list(const OSTask* task, uint32_t elapsed_us) {
        if (display_list_count++ == 0) {
            hm64vita::log_line("checkpoint: first display list received (task type %u)", (unsigned int)task->t.type);
        }
        display_list_us += elapsed_us;
        raise_max(display_list_max_us, elapsed_us);
    }

    void note_screen_update(uint32_t elapsed_us) {
        if (screen_update_count++ == 0) {
            hm64vita::log_line("checkpoint: first screen update (VI)");
        }
        screen_update_us += elapsed_us;
        raise_max(screen_update_max_us, elapsed_us);
    }

    RendererTimes take_renderer_times() {
        RendererTimes times;
        times.display_list_us = display_list_us.load();
        times.display_list_max_us = display_list_max_us.exchange(0);
        times.screen_update_us = screen_update_us.load();
        times.screen_update_max_us = screen_update_max_us.exchange(0);
        return times;
    }

    class NullRenderContext : public ultramodern::renderer::RendererContext {
    public:
        NullRenderContext() {
            hm64vita::log_line("checkpoint: null render context created");
            setup_result = ultramodern::renderer::SetupResult::Success;
        }

        bool valid() override {
            return true;
        }

        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override {
            return false;
        }

        void enable_instant_present() override {}

        void send_dl(const OSTask* task) override {
            note_display_list(task, 0);
        }

        void send_dummy_workload(uint32_t fb_address) override {}

        void update_screen() override {
            note_screen_update(0);
        }

        void shutdown() override {}

        uint32_t get_display_framerate() const override {
            return 60;
        }

        float get_resolution_scale() const override {
            return 1.0f;
        }
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        return std::make_unique<NullRenderContext>();
    }

    uint32_t get_display_list_count() {
        return display_list_count.load();
    }

    uint32_t get_screen_update_count() {
        return screen_update_count.load();
    }
}
