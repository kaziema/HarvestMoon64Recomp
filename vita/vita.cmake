# PS Vita build of Harvest Moon 64: Recompiled.
#
# Phase 0 (null renderer): RT64 and RecompFrontend (recompui, recompinput) are not built.
# Display lists are accepted and dropped so the CPU cost of the game code can be measured.
#
# Configure with:
#   vita/build_host_tools.sh
#   cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake

if (NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set. Configure inside the VitaSDK environment.")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(HM64_VITA_TITLEID "HMRC00064" CACHE STRING "Nine character Vita title ID")
set(HM64_VITA_NAME "Harvest Moon 64" CACHE STRING "Name under the LiveArea bubble")
set(HM64_VITA_VERSION "00.01" CACHE STRING "Vita app version")

# Host tools. These run on the build machine, so they cannot come from this cross build.
set(HM64_HOST_TOOLS_DIR "${CMAKE_SOURCE_DIR}/vita/host-tools/bin" CACHE PATH "Directory with host-built file_to_c, N64Recomp and RSPRecomp")
find_program(HM64_FILE_TO_C file_to_c PATHS ${HM64_HOST_TOOLS_DIR} NO_DEFAULT_PATH)
find_program(HM64_N64RECOMP N64Recomp PATHS ${HM64_HOST_TOOLS_DIR} NO_DEFAULT_PATH)
if (NOT HM64_FILE_TO_C OR NOT HM64_N64RECOMP)
    message(FATAL_ERROR "Host tools not found in ${HM64_HOST_TOOLS_DIR}. Run vita/build_host_tools.sh first.")
endif()

# Vita changes to N64ModernRuntime and N64Recomp, kept as patch files (see vita/apply_patches.cmake).
include(${CMAKE_SOURCE_DIR}/vita/apply_patches.cmake)

set(BUILD_SHARED_LIBS OFF)

# fmt's file/OS helpers need fdopen and fileno, which newlib on the Vita does not provide.
set(FMT_OS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

# The mod live recompiler (sljit) allocates executable memory with mmap, which the Vita does not have.
# Mods are not supported on the Vita, so its executable allocator is replaced with one that always fails.
# The settings live in vita/sljit/sljitConfigPre.h, which sljit includes when SLJIT_HAVE_CONFIG_PRE is set.
target_compile_definitions(LiveRecomp PUBLIC SLJIT_HAVE_CONFIG_PRE=1)
target_include_directories(LiveRecomp PUBLIC ${CMAKE_SOURCE_DIR}/vita/sljit)

# RecompiledFuncs: the primary recompiler output (generated from the ROM with N64Recomp).
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)

target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# PatchesLib: the recompiled output of the game's custom function patches.
add_library(PatchesLib STATIC)

target_compile_options(PatchesLib PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

target_include_directories(PatchesLib PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

target_sources(PatchesLib PRIVATE
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
    ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
)

set_source_files_properties(${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c PROPERTIES COMPILE_FLAGS -fno-strict-aliasing)

# Patches are built for MIPS with a host clang + ld.lld, then recompiled with the host N64Recomp.
# Apple's clang cannot target MIPS, so prefer Homebrew's llvm and lld when they are installed.
if (NOT DEFINED PATCHES_C_COMPILER)
    if (EXISTS /opt/homebrew/opt/llvm/bin/clang)
        set(PATCHES_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
    else()
        set(PATCHES_C_COMPILER clang)
    endif()
endif()

if (NOT DEFINED PATCHES_LD)
    if (EXISTS /opt/homebrew/opt/lld/bin/ld.lld)
        set(PATCHES_LD /opt/homebrew/opt/lld/bin/ld.lld)
    else()
        set(PATCHES_LD ld.lld)
    endif()
endif()

# Newer clang releases turn incompatible pointer type warnings in the game's patch code into errors.
set(PATCHES_CC "${PATCHES_C_COMPILER} -Wno-error=incompatible-pointer-types")

add_custom_target(PatchesBin
    COMMAND ${CMAKE_COMMAND} -E env "CC=${PATCHES_CC}" LD=${PATCHES_LD} make
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/patches
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/patches/patches.elf
)

add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c
    COMMAND ${HM64_FILE_TO_C} ${CMAKE_SOURCE_DIR}/patches/patches.bin game_patches_bin ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.c ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches_bin.h
    DEPENDS ${CMAKE_SOURCE_DIR}/patches/patches.bin
)

add_custom_command(OUTPUT
                       ${CMAKE_SOURCE_DIR}/patches/patches.bin
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/patches.c
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/recomp_overlays.inl
                       ${CMAKE_SOURCE_DIR}/RecompiledPatches/funcs.h
                   COMMAND ${HM64_N64RECOMP} patches.toml
                   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                   DEPENDS ${CMAKE_SOURCE_DIR}/patches/patches.elf
)

# Main executable.
add_executable(HarvestMoon64Vita)

target_sources(HarvestMoon64Vita PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vita/main_vita.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/null_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_api.cpp

    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp

    ${CMAKE_SOURCE_DIR}/src/game/debug.cpp
    ${CMAKE_SOURCE_DIR}/src/game/rom_decompression.cpp
    ${CMAKE_SOURCE_DIR}/src/game/unknown_symbols.cpp

    ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp
)

target_include_directories(HarvestMoon64Vita PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/vita
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

target_compile_options(HarvestMoon64Vita PRIVATE
    -fno-strict-aliasing
)

target_link_libraries(HarvestMoon64Vita PRIVATE
    PatchesLib
    RecompiledFuncs
    librecomp
    ultramodern
    pthread
)

# Linker map, used to check that patched functions override the original recompiled ones.
target_link_options(HarvestMoon64Vita PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/HarvestMoon64Vita.map)

vita_create_self(eboot.bin HarvestMoon64Vita UNSAFE)

# Extended memory budget (+109 MiB main RAM), the same setting my other ports use.
if (NOT VITA_MKSFOEX_FLAGS MATCHES "ATTRIBUTE2")
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
endif()

vita_create_vpk(HarvestMoon64Vita.vpk ${HM64_VITA_TITLEID} eboot.bin
    VERSION ${HM64_VITA_VERSION}
    NAME ${HM64_VITA_NAME}
)
