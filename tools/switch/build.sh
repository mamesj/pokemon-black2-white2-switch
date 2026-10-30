#!/usr/bin/env bash
# Builds ssrecomp for the Nintendo Switch (an .nro for the Homebrew Menu) from your own ROM.
#
#   tools/switch/build.sh [your-soulsilver-us.nds]
#
# Needs devkitPro with the switch-dev and switch-sdl2 packages (see README, "Nintendo
# Switch"), CMake and Ninja, and Python 3 to generate the game's code. devkitPro's
# devkitpro/devkita64 Docker image has all of these. The ROM is only needed the first time,
# or when gen/ is missing: the recompiled code in gen/ is the same for every platform and
# is shared with the PC build.
#
# The result, build-switch/ssrecomp.nro, holds your game's code: keep it to yourself.
#
# SSR_SWITCH_JIT=1: a build with melonDS's JIT (EMULATION in the pause menu), from the folder
# of generated tables given in SSR_SWITCH_GEN (e.g. gen-tables, made by
# tools/soulsilver/tables_only.py: everything but the recompiled code, so the build takes a
# couple of minutes and the .nro is small), into build-switch-jit unless SSR_SWITCH_BUILD says
# otherwise. SSR_NRO_TITLE: the title in the Homebrew Menu (ssrecomp by default). SSR_SWITCH_ROMFS:
# a folder for the .nro's romfs (the all-games build's function tables for the Black games).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
JIT="${SSR_SWITCH_JIT:-0}"
if [ "$JIT" = 1 ]; then
    BUILD="${SSR_SWITCH_BUILD:-build-switch-jit}"
    GEN="${SSR_SWITCH_GEN:-$HERE/gen}"
else
    BUILD="${SSR_SWITCH_BUILD:-build-switch}"
    GEN="$HERE/gen"
fi

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

# The toolchain.
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] ||
    fail "devkitPro's Switch toolchain is not in $DEVKITPRO. Install devkitPro, then: sudo dkp-pacman -S switch-dev switch-sdl2"
[ -f "$DEVKITPRO/portlibs/switch/lib/libSDL2.a" ] ||
    fail "SDL2 for the Switch is missing: sudo dkp-pacman -S switch-sdl2"
for tool in cmake ninja; do
    command -v "$tool" >/dev/null || fail "$tool is not installed (sudo apt install cmake ninja-build)"
done

# The game's code, generated from the ROM (once).
if [ ! -f gen/sources.cmake ]; then
    ROM="${1:-}"
    [ -n "$ROM" ] || fail "gen/ is missing: give your SoulSilver (US) ROM, e.g. tools/switch/build.sh ~/soulsilver.nds"
    [ -f "$ROM" ] || fail "no such file: $ROM"
    command -v python3 >/dev/null || fail "python3 is not installed (sudo apt install python3)"
    say "Recompiling the game's code from $ROM"
    python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
    python3 tools/apps.py --layout layouts/soulsilver-us.json gen
fi

# About 1.5 GB of memory per compile job (the generated files are large).
JOBS="${SSR_JOBS:-}"
if [ -z "$JOBS" ]; then
    avail_kb=$(awk '/MemAvailable/ { print $2 }' /proc/meminfo 2>/dev/null || echo 0)
    JOBS=$(( avail_kb / 1572864 ))
    cpus=$(nproc 2>/dev/null || echo 1)
    [ "$JOBS" -gt "$cpus" ] && JOBS=$cpus
    [ "$JOBS" -lt 1 ] && JOBS=1
fi

# devkitPro somewhere other than /opt/devkitpro (unpacked from its Docker image, as Pokemon GBA
# Widescreen's tools/switch/build.sh does): its pkg-config files still say /opt/devkitpro, so a
# pkg-config that puts the real folder in front of their paths.
PKGCONF=()
if [ "$(realpath -m "$DEVKITPRO")" != /opt/devkitpro ]; then
    mkdir -p "$BUILD"
    cat > "$BUILD/pkg-config-switch" <<PKG
#!/bin/sh
export PKG_CONFIG_DIR= PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$DEVKITPRO/portlibs/switch/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="${DEVKITPRO%/opt/devkitpro}"
exec pkg-config --static "\$@"
PKG
    chmod +x "$BUILD/pkg-config-switch"
    PKGCONF=(-DPKG_CONFIG_EXECUTABLE="$HERE/$BUILD/pkg-config-switch")
fi
# A build folder made with devkitPro in another place (in Docker, say) starts again: CMake keeps
# the compilers it found the first time.
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q "^CMAKE_TOOLCHAIN_FILE:[A-Z]*=$DEVKITPRO/cmake/Switch.cmake\$" "$BUILD/CMakeCache.txt"; then
    say "devkitPro is somewhere else now: configuring $BUILD from the start"
    rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
fi

say "Configuring ($BUILD)"
if [ "$JIT" = 1 ]; then
    # (Configured every time, so that an older build folder picks up new options.)
    [ -f "$GEN/sources.cmake" ] || fail "no generated tables in $GEN"
    cmake -S . -B "$BUILD" -G Ninja "${PKGCONF[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSSR_GENERATED_DIR="$GEN" \
        -DSSR_NRO_TITLE="${SSR_NRO_TITLE:-ssrecomp}" \
        -DSSR_SWITCH_ROMFS="${SSR_SWITCH_ROMFS:-}" \
        -DSSR_ALL_GAMES="${SSR_ALL_GAMES:-OFF}" \
        -DSSR_PC_ART="${SSR_PC_ART:-}" \
        -DSSR_GBA_DIR="${SSR_GBA_DIR:-}" \
        -DSSR_JIT=ON
elif [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S . -B "$BUILD" -G Ninja "${PKGCONF[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSSR_GENERATED_DIR="$HERE/gen"
fi

say "Compiling with $JOBS jobs (the first time takes a while)"
ninja -C "$BUILD" -j "$JOBS" ssrecomp_nro

NRO="$BUILD/ssrecomp.nro"
[ -f "$NRO" ] || fail "the build finished but $NRO was not made"
say "Done: $NRO ($(du -h "$NRO" | cut -f1))"
cat <<EOF

On the Switch's SD card, make the folder /switch/ssrecomp/ and copy into it:
  - $NRO
  - your ROM, renamed to rom.nds
Then hold R while starting any game, and choose ssrecomp in the Homebrew Menu.
The save (soulsilver.sav), settings (ssrecomp.cfg) and game.log are written to that folder.
EOF
