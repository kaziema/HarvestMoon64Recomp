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

# Phase 0 step 2: run RT64's front end on a null Plume backend. OFF uses the null renderer (display lists dropped).
option(HM64_VITA_RT64 "Run RT64 on the null Plume backend instead of the null renderer" ON)
if (HM64_VITA_RT64)
    # VV core (libvv): the GXM layer rt64-gxm draws through.
    set(HM64_LIBVV_DIR "${CMAKE_SOURCE_DIR}/../libvv" CACHE PATH "The libvv repo (VV core)")
    add_subdirectory(${HM64_LIBVV_DIR}/core ${CMAKE_BINARY_DIR}/libvv_core)
    include(${CMAKE_SOURCE_DIR}/vita/rt64.cmake)
endif()

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

# Build stamp, rewritten on every build so the boot log always identifies the exact binary.
set(HM64_BUILD_STAMP_C ${CMAKE_BINARY_DIR}/build_stamp.c)
add_custom_target(HM64BuildStamp
    COMMAND ${CMAKE_COMMAND} -DOUT=${HM64_BUILD_STAMP_C} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/vita/build_stamp.cmake
    BYPRODUCTS ${HM64_BUILD_STAMP_C}
    COMMENT "Writing build stamp"
)

# Main executable.
add_executable(HarvestMoon64Vita)
add_dependencies(HarvestMoon64Vita HM64BuildStamp)

target_sources(HarvestMoon64Vita PRIVATE
    ${CMAKE_SOURCE_DIR}/src/vita/main_vita.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/null_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_api.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_log.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_selftest.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_stats.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/vita/vita_timeline.cpp
    ${HM64_BUILD_STAMP_C}

    ${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/main/register_patches.cpp

    ${CMAKE_SOURCE_DIR}/src/game/debug.cpp
    ${CMAKE_SOURCE_DIR}/src/game/rom_decompression.cpp
    ${CMAKE_SOURCE_DIR}/src/game/unknown_symbols.cpp

    ${CMAKE_SOURCE_DIR}/rsp/n_aspMain.cpp
)

if (HM64_VITA_RT64)
    target_sources(HarvestMoon64Vita PRIVATE
        ${CMAKE_SOURCE_DIR}/src/vita/plume_null.cpp
        ${CMAKE_SOURCE_DIR}/src/vita/vita_rt64_context.cpp
        ${CMAKE_SOURCE_DIR}/src/vita/vita_rt64_compat.cpp
        ${CMAKE_SOURCE_DIR}/src/vita/vita_rt64_stubs.cpp
        ${CMAKE_SOURCE_DIR}/src/vita/rt64_vita_profile.cpp
    )
    target_include_directories(HarvestMoon64Vita PRIVATE ${CMAKE_SOURCE_DIR}/vita/rt64)
    target_compile_definitions(HarvestMoon64Vita PRIVATE HM64_VITA_RT64)
    target_link_libraries(HarvestMoon64Vita PRIVATE rt64)
endif()

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
    SceIofilemgr_stub
    SceKernelThreadMgr_stub
    SceSysmem_stub
    SceLibKernel_stub
    ScePower_stub
    SceCtrl_stub
    SceAudio_stub
)

# libstdc++ references its pthread functions weakly. When nothing else pulls one in from libpthread it stays
# undefined, and the ARM linker turns calls to it into no-ops. -u forces each one to be linked.
#   pthread_cancel: libstdc++ only allows std::thread when it resolves (TEST 1 crash: every std::thread threw).
#   pthread_once: creates the mutex that guards function-local statics (TEST 3 crash: the call was a no-op,
#     so __cxa_guard_acquire locked a null mutex).
#   pthread_cond_wait/broadcast/destroy: used by the same static guard when two threads initialize at once.
# --wrap=pthread_create routes every thread creation through the logging wrapper in vita_log.cpp.
target_link_options(HarvestMoon64Vita PRIVATE
    -Wl,-u,pthread_cancel
    -Wl,-u,pthread_once
    -Wl,-u,pthread_cond_wait
    -Wl,-u,pthread_cond_broadcast
    -Wl,-u,pthread_cond_destroy
    -Wl,--wrap=pthread_create
)

# Fails the build if any pthread or sched function is still an undefined weak symbol, so this class of bug
# shows up at build time instead of as a crash on hardware.
add_custom_command(TARGET HarvestMoon64Vita POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DELF=$<TARGET_FILE:HarvestMoon64Vita> -P ${CMAKE_SOURCE_DIR}/vita/check_weak_symbols.cmake
    VERBATIM
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
