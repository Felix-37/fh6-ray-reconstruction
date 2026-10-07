#!/usr/bin/env bash
# Builds rr-forza.addon64 and the rr_metrics tool with llvm-mingw + CMake + Ninja (Git Bash on Windows,
# or Linux). The tools are taken from PATH. If they live elsewhere, create build.local.sh next to this
# script (ignored by git), for example:
#   LLVM_MINGW=/d/tools/llvm-mingw
#   CMAKE=/d/tools/cmake/bin/cmake.exe
#   NINJA=/d/tools/ninja/ninja.exe
# Output: build/rr-forza.addon64. See docs/BUILDING.md.
set -e
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${BUILD_DIR:-$SRC/build}"
if [ -f "$SRC/build.local.sh" ]; then
  . "$SRC/build.local.sh"
fi
if [ -n "$LLVM_MINGW" ]; then
  export PATH="$LLVM_MINGW/bin:$PATH"
fi
if [ -n "$NINJA" ]; then
  export PATH="$(dirname "$NINJA"):$PATH"
fi
CMAKE="${CMAKE:-cmake}"
if ! command -v x86_64-w64-mingw32-clang++ >/dev/null 2>&1; then
  echo "llvm-mingw not found (x86_64-w64-mingw32-clang++). Install it or set LLVM_MINGW in build.local.sh (docs/BUILDING.md)." >&2
  exit 1
fi
if [ ! -f "$BUILD/build.ninja" ]; then
  "$CMAKE" -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/mingw-w64-x86_64.cmake"
fi
"$CMAKE" --build "$BUILD"
