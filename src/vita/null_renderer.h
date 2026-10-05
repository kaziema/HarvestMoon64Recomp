#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace hm64vita {
    // Phase 0 renderer: accepts every display list and draws nothing.
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    // Totals since startup, for Phase 0 timing logs.
    uint32_t get_display_list_count();
    uint32_t get_screen_update_count();
}
