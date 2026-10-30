#!/usr/bin/env bash
# Builds the HOME Menu entries' program (tools/switch/forwarder) into a folder, for the all-games
# program's romfs: tools/switch/build_forwarder.sh <build folder> <out folder>
# Run with devkitPro's tools (DEVKITPRO set, or inside its Docker image: build_game.sh does that).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
# (A build folder made with devkitPro in another place, in Docker say, starts again: CMake keeps
# the compilers it found the first time.)
if [ -f "$1/CMakeCache.txt" ] && ! grep -q "^CMAKE_TOOLCHAIN_FILE:[A-Z]*=$DEVKITPRO/cmake/Switch.cmake\$" "$1/CMakeCache.txt"; then
    rm -rf "$1/CMakeCache.txt" "$1/CMakeFiles"
fi
cmake -S "$HERE/tools/switch/forwarder" -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DSSR_FORWARDER_OUT="$2" >/dev/null
ninja -C "$1" exefs
ls -l "$2"
