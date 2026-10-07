# RT64 for the PS Vita, built from the rt64-gxm repo (a fork of RT64 at the commit this port pins, f0d8c9f,
# with the Vita changes as ordinary edits).
#
# Phase 0 step 2: RT64's front end runs for real (display list interpreter, GBI, RSP/RDP recording, framebuffer
# pairs, full sync, worker threads) on a null Plume backend (src/vita/plume_null.cpp), so its CPU cost can be
# measured with no GPU work. In Phase 1 the same seam takes the GXM backend.
#
# The precompiled SPIR-V shader blobs are built on the host first: vita/build_rt64_shaders.sh

set(HM64_RT64_GXM_DIR "${CMAKE_SOURCE_DIR}/../rt64-gxm" CACHE PATH "The rt64-gxm repo (RT64 fork with the Vita GXM renderer)")
set(RT64_DIR ${HM64_RT64_GXM_DIR})
if (NOT EXISTS ${RT64_DIR}/src/hle/rt64_application.cpp)
    message(FATAL_ERROR "rt64-gxm not found at ${RT64_DIR}. Set HM64_RT64_GXM_DIR.")
endif()
set(RT64_SHADER_BUILD_DIR ${CMAKE_SOURCE_DIR}/vita/host-tools/rt64-shaders-host)

if (NOT EXISTS ${RT64_SHADER_BUILD_DIR}/src/shaders/RenderParams.hlsli.rw.c)
    message(FATAL_ERROR "RT64 shader blobs not found in ${RT64_SHADER_BUILD_DIR}. Run vita/build_rt64_shaders.sh first.")
endif()

# Source list, read from RT64's own CMakeLists.txt so it follows upstream.
file(READ ${RT64_DIR}/CMakeLists.txt RT64_CMAKE_TEXT)
string(REGEX MATCH "set \\(SOURCES[^)]*\\)" RT64_SOURCES_BLOCK "${RT64_CMAKE_TEXT}")
string(REGEX MATCHALL "\"[^\"]+\"" RT64_SOURCES_QUOTED "${RT64_SOURCES_BLOCK}")
set(RT64_VITA_SOURCES)
foreach(source IN LISTS RT64_SOURCES_QUOTED)
    string(REPLACE "\"" "" source "${source}")
    string(REPLACE "\${PROJECT_SOURCE_DIR}" "${RT64_DIR}" source "${source}")
    list(APPEND RT64_VITA_SOURCES "${source}")
endforeach()
list(LENGTH RT64_VITA_SOURCES RT64_SOURCE_COUNT)
if (RT64_SOURCE_COUNT LESS 50)
    message(FATAL_ERROR "Could not read RT64's source list from ${RT64_DIR}/CMakeLists.txt (found ${RT64_SOURCE_COUNT}).")
endif()

# Not built on the Vita: the Vulkan backend for ImGui.
list(REMOVE_ITEM RT64_VITA_SOURCES
    "${RT64_DIR}/src/contrib/imgui/backends/imgui_impl_vulkan.cpp"
)

# Precompiled shader blobs (SPIR-V) and the RenderParams text, generated on the host.
file(GLOB RT64_SHADER_BLOB_SOURCES
    ${RT64_SHADER_BUILD_DIR}/src/shaders/*.spirv.c
    ${RT64_SHADER_BUILD_DIR}/src/shaders/*.rw.c
)

# re-spirv: RT64's SPIR-V optimizer, used to specialize the raster shaders.
add_library(rt64_respirv STATIC ${RT64_DIR}/src/contrib/re-spirv/re-spirv.cpp)
target_include_directories(rt64_respirv PUBLIC
    ${RT64_DIR}/src/contrib/re-spirv
    ${RT64_DIR}/src/contrib/re-spirv/external/SPIRV-Headers/include
)
# RT64 and re-spirv are C++17 code (C++20 changes path::u8string() and aggregate rules they rely on).
set_target_properties(rt64_respirv PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_library(rt64 STATIC ${RT64_VITA_SOURCES} ${RT64_SHADER_BLOB_SOURCES})

# rt64-gxm: RT64's GPU half for the Vita, drawing the recorded frames through VV core (libvv).
target_sources(rt64 PRIVATE ${RT64_DIR}/src/gxm/rt64_gxm_renderer.cpp ${RT64_DIR}/src/gxm/rt64_gxm_textures.cpp)
target_compile_definitions(rt64 PUBLIC RT64_GXM)
target_link_libraries(rt64 PUBLIC vv_core)

set_target_properties(rt64 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# newlib on the Vita has no timegm (implot uses it).
set_source_files_properties(${RT64_DIR}/src/contrib/implot/implot.cpp PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/vita/rt64/vita_rt64_compat.h"
)

target_compile_definitions(rt64 PUBLIC
    HLSL_CPU
    FFX_GCC
)

# Phase 0 profiling zones (TEST 9), see rt64-gxm src/vita/rt64_vita_profile.h.
target_compile_definitions(rt64 PRIVATE RT64_VITA_PROFILE)
target_include_directories(rt64 PUBLIC ${RT64_DIR}/src/vita)

target_include_directories(rt64 PUBLIC
    ${RT64_DIR}/src
    ${RT64_DIR}/src/contrib
    ${RT64_DIR}/src/contrib/plume
    ${RT64_DIR}/src/contrib/imgui
    ${RT64_DIR}/src/contrib/hlslpp/include
    ${RT64_DIR}/src/contrib/mupen64plus-core/src/api
    ${RT64_DIR}/src/contrib/nativefiledialog-extended/src/include
    ${RT64_SHADER_BUILD_DIR}/src
    $ENV{VITASDK}/arm-vita-eabi/include/SDL2
)

target_link_libraries(rt64 PUBLIC rt64_respirv zstd)
