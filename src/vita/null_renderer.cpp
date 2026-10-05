#include "null_renderer.h"

#include <atomic>

namespace hm64vita {
    static std::atomic<uint32_t> display_list_count = 0;
    static std::atomic<uint32_t> screen_update_count = 0;

    class NullRenderContext : public ultramodern::renderer::RendererContext {
    public:
        NullRenderContext() {
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
            display_list_count++;
        }

        void send_dummy_workload(uint32_t fb_address) override {}

        void update_screen() override {
            screen_update_count++;
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
