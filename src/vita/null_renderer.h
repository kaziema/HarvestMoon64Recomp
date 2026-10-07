#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace hm64vita {
    // Phase 0 renderer: accepts every display list and draws nothing.
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // Totals since startup, for Phase 0 timing logs.
    uint32_t get_display_list_count();
    uint32_t get_screen_update_count();

    // Called by each renderer (null or RT64) for every display list and screen update, with the time it took.
    void note_display_list(const OSTask* task, uint32_t elapsed_us);
    void note_screen_update(uint32_t elapsed_us);

    // Renderer time totals since startup and the longest single call, in microseconds (the max resets on read).
    struct RendererTimes {
        uint64_t display_list_us;
        uint32_t display_list_max_us;
        uint64_t screen_update_us;
        uint32_t screen_update_max_us;
    };
    RendererTimes take_renderer_times();
}
