#pragma once

#include <memory>

#include "ultramodern/renderer_context.hpp"

namespace hm64vita {
    // RT64 render context for the Vita: a copy of RecompFrontend's (recompui/src/renderer/rt64_render_context.cpp)
    // without recompui and texture packs. Phase 0 runs it on the null Plume backend (plume_null.cpp).
    std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}
