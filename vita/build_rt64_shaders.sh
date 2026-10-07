#!/bin/sh
# Builds RT64's precompiled shader blobs (SPIR-V, embedded as C arrays) on the host, for the Vita build of RT64.
#
# The Vita build does not run any of these shaders on a GPU: Phase 0 uses a null Plume backend. RT64 still needs
# the real SPIR-V, because it runs its SPIR-V optimizer (re-spirv) on the raster shaders and asserts if they are not
# valid. The blobs come from RT64's own CMake rules and its bundled macOS DXC, so they match upstream exactly.
#
# RT64's shader commands break when the path contains a space (the repositories live under "vita ports"), so the
# build goes through temporary symlinks without spaces. The shaders come from the rt64-gxm repo (next to this one,
# or set RT64_GXM_DIR).
#
# Output: vita/host-tools/rt64-shaders-host/src/shaders/*.spirv.c|h and RenderParams.hlsli.rw.c|h
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
RT64_GXM="${RT64_GXM_DIR:-$REPO/../rt64-gxm}"
RT64_GXM="$(cd "$RT64_GXM" && pwd)"
LINKS="$(mktemp -d)"
ln -s "$REPO" "$LINKS/hm64"
ln -s "$RT64_GXM" "$LINKS/rt64-gxm"
BUILD="$LINKS/hm64/vita/host-tools/rt64-shaders-host"

env -u VITASDK cmake -S "$LINKS/rt64-gxm" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRT64_STATIC=ON

cd "$BUILD"
grep -oE "^build src/shaders/[A-Za-z0-9_]+\.(hlsl|hlsli)\.(spirv|rw)\.c" build.ninja | sed 's/^build //' | sort -u | xargs ninja

rm "$LINKS/hm64" "$LINKS/rt64-gxm"
echo "RT64 shader blobs are in $REPO/vita/host-tools/rt64-shaders-host/src/shaders"
