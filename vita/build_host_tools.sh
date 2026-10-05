#!/usr/bin/env bash
# Builds the host tools the Vita build needs, using the build machine's own compiler:
#   file_to_c   (from lib/rt64/src/tools/file_to_c)
#   N64Recomp   (from lib/N64ModernRuntime/N64Recomp)
#   RSPRecomp   (from lib/N64ModernRuntime/N64Recomp)
# Output: vita/host-tools/bin
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/vita/host-tools"
BIN="$OUT/bin"
mkdir -p "$BIN"

# Make sure no cross toolchain leaks into the host builds.
unset CC CXX

cmake -S "$ROOT/lib/rt64/src/tools/file_to_c" -B "$OUT/build/file_to_c" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/build/file_to_c"
cp "$OUT/build/file_to_c/file_to_c" "$BIN/"

cmake -S "$ROOT/lib/N64ModernRuntime/N64Recomp" -B "$OUT/build/N64Recomp" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/build/N64Recomp" --target N64RecompCLI RSPRecomp
cp "$OUT/build/N64Recomp/N64Recomp" "$OUT/build/N64Recomp/RSPRecomp" "$BIN/"

echo "Host tools built in $BIN"
