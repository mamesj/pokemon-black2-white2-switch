#!/usr/bin/env bash
# Builds the PC all-games download: one program for every game (Pokemon Black, White, Black 2,
# White 2, SoulSilver, HeartGold and Platinum) with a game list when it starts
# (frontend/pc_launcher.cpp). The player puts their ROMs in its roms folder and picks a game.
#
#   tools/pc/build_all.sh windows your-soulsilver-us.nds
#   tools/pc/build_all.sh linux   your-soulsilver-us.nds
#   tools/pc/build_all.sh both    your-soulsilver-us.nds
#   tools/pc/build_all.sh mac     your-soulsilver-us.nds    (on a Mac)
#
# As for the Switch's all-games download (tools/switch/build_game.sh all), the SoulSilver ROM is
# only read for the addresses in SoulSilver and HeartGold's tables, and the Black games' function
# tables come from the decompilations' symbol files: the downloads hold nothing of the games.
#
# The results, in build-pc-dist/:
#   pokemon-ds-wide-pc-windows-x86_64.zip   Pokemon DS Launcher.exe, SDL2.dll, data/, roms/
#   pokemon-ds-wide-pc-linux-x86_64.tar.gz  pokemon-ds-wide, libSDL2-2.0.so.0, data/, roms/
#   pokemon-ds-wide-pc-macos-arm64.zip      Pokemon DS Launcher.app, data/, roms/
#
# The Mac build needs a Mac with the Xcode Command Line Tools, git, CMake, Ninja, Python 3 and
# curl (Homebrew: brew install cmake ninja); SDL2 is built into the program.
# The others need Linux or WSL2 (Ubuntu 24.04) with git, CMake, Ninja, Python 3 and curl; for Windows also
# MinGW-w64 (sudo apt install g++-mingw-w64-x86-64-posix), for Linux SDL2's development files
# (sudo apt install libsdl2-dev). SDL2 for Windows is SDL's own MinGW package, fetched once into
# build-pc-deps/ and checked against its known checksum. The Windows program's icon:
# SSR_PC_ICON=your.ico (optional). The game list's pictures, built into the program: SSR_PC_ART, a
# folder of your own with IREO.jpg, IRDO.jpg ... named as for the list's icons/ folder (optional).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
TARGET="${1:-}"
ROM="${2:-}"
BW2_DECOMP=https://github.com/fuddlesworth/pokebw2.git
BW2_COMMIT=753b3fd97683fc619bb98b52e7276afac3d5e32f
W2_COMMIT=be2c125a700c23702ba1c5a4ef63e0e970bcc87a
BW1_DECOMP=https://github.com/Project-Holon/aletheia.git
BW1_COMMIT=0d05b6ccc61fcea090d8318a00af9f1634810e18
SDL_VERSION=2.30.9
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
SDL_SHA256=b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c
# (Linux: SDL2's source, built here and put in the download; see "Compiling for Linux".)
SDL_SRC_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
SDL_SRC_SHA256=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

WINDOWS=0 LINUX=0 MAC=0
case "$TARGET" in
    windows) WINDOWS=1 ;;
    linux) LINUX=1 ;;
    both) WINDOWS=1 LINUX=1 ;;
    mac) MAC=1 ;;
    *) fail "which? tools/pc/build_all.sh windows|linux|both|mac your-soulsilver-us.nds" ;;
esac
[ "$MAC" = 0 ] || [ "$(uname -s)" = Darwin ] || fail "the Mac program is built on a Mac"
[ -n "$ROM" ] || fail "give your SoulSilver ROM (US): tools/pc/build_all.sh $TARGET your-soulsilver-us.nds"
[ -f "$ROM" ] || fail "no such file: $ROM"
[ "$(dd if="$ROM" bs=1 skip=12 count=4 2>/dev/null)" = IPGE ] ||
    fail "$ROM is not Pokemon SoulSilver (US, game code IPGE): the tables are made from it"
ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"
for tool in git cmake ninja python3 curl; do
    command -v "$tool" >/dev/null || fail "$tool is not installed (Ubuntu: sudo apt install git cmake ninja-build python3 curl)"
done
if [ "$WINDOWS" = 1 ]; then
    command -v x86_64-w64-mingw32-g++-posix >/dev/null ||
        fail "MinGW-w64 is not installed (Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix)"
fi
if [ "$LINUX" = 1 ]; then
    { pkg-config --exists sdl2 2>/dev/null || [ -f /usr/include/SDL2/SDL.h ]; } ||
        fail "SDL2's development files are not installed (Ubuntu: sudo apt install libsdl2-dev)"
fi

# The symbol files of a decompilation at a fixed version, without the rest of it.
fetch_symbols() { # dir url commit paths...
    local dir="$1" url="$2" commit="$3"
    shift 3
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" sparse-checkout set --no-cone "$@"
    git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit"
    git -C "$dir" checkout -q --force FETCH_HEAD
}

# SoulSilver and HeartGold's tables (addresses only), as for the Switch.
if [ ! -f gen/sources.cmake ]; then
    say "SoulSilver's tables, from $ROM"
    python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
    python3 tools/apps.py --layout layouts/soulsilver-us.json gen
fi
python3 tools/soulsilver/tables_only.py gen build-gen-tables

# The function tables (made without a ROM: the program reads the few bytes they need from the
# player's ROM), one per game, in data/funcs beside the program.
DATA="$HERE/build-pc-data"
rm -rf "$DATA"
mkdir -p "$DATA/funcs"
say "The function tables, from the decompilations' symbol files"
fetch_symbols build-decomp-pokebw2 "$BW2_DECOMP" "$BW2_COMMIT" \
    '/config/b2_us/arm9/symbols.txt' '/config/b2_us/arm9/*/symbols.txt' '/config/b2_us/arm9/overlays/*/symbols.txt'
fetch_symbols build-decomp-pokew2 "$BW2_DECOMP" "$W2_COMMIT" \
    '/config/w2_us/arm9/symbols.txt' '/config/w2_us/arm9/*/symbols.txt' '/config/w2_us/arm9/overlays/*/symbols.txt'
fetch_symbols build-decomp-aletheia "$BW1_DECOMP" "$BW1_COMMIT" '/layout/function_addrs.tsv'
python3 tools/bw2/funcmap.py build-decomp-pokebw2/config/b2_us/arm9 - "$DATA/funcs/IREO.txt"
python3 tools/bw2/funcmap.py build-decomp-pokew2/config/w2_us/arm9 - "$DATA/funcs/IRDO.txt"
python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$DATA/funcs/IRBO.txt"
python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$DATA/funcs/IRAO.txt" --white
cp tools/platinum/CPUE.txt "$DATA/funcs/CPUE.txt"

# The build: the JIT for every game, no libpng (screenshots are for tests), OpenGL for WORLD ON
# GPU (WGL on Windows, EGL on Linux), and SSE rather than the building machine's AVX2
# (SSR_PORTABLE_X86), so that it runs on any 64-bit PC.
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DSSR_JIT=ON -DSSR_PC_ALL_GAMES=ON -DSSR_PNG=OFF -DSSR_GPU3D=ON
        -DSSR_PORTABLE_X86=ON -DSSR_GENERATED_DIR="$HERE/build-gen-tables")
ART="${SSR_PC_ART:-}"
[ -n "$ART" ] && { [ -d "$ART" ] || fail "SSR_PC_ART: no such folder: $ART"; ART="$(realpath "$ART")"; say "Pictures for the game list: $ART"; }
COMMON+=(-DSSR_PC_ART="$ART")
mkdir -p build-pc-dist

# The folder the player unpacks: the program, its function tables, an empty roms folder and the
# notes. $1: the folder, $2: the program's file name.
stage() {
    local dir="$1"
    rm -rf "$dir"
    mkdir -p "$dir/roms" "$dir/data"
    cp -r "$DATA/funcs" "$dir/data/funcs"
    cp tools/pc/README.txt "$dir/README.txt"
    cp LICENSE "$dir/LICENSE.txt"
}

# Only these files may be in a download: the program, SDL2 and its licence, the function tables,
# the notes, the licence and the empty roms folder; never a ROM, save, save state or game code.
check_contents() { # list of paths, one per line, relative to the download's top folder
    python3 -c "
import re, sys
ok = re.compile(r'^pokemon-ds-wide/(pokemon-ds-wide|Pokemon DS Launcher\.exe|SDL2\.dll|libSDL2-2\.0\.so\.0|LICENSE\.txt|LICENSE-SDL2\.txt|README\.txt|roms/|data/|data/funcs/|data/funcs/(IREO|IRDO|IRBO|IRAO|CPUE)\.txt|Pokemon DS Launcher\.app/(Contents/(Info\.plist|MacOS/(pokemon-ds-wide)?|_CodeSignature/(CodeResources)?)?)?)?$')
bad = [l for l in sys.stdin.read().splitlines() if l and not ok.match(l)]
if bad: sys.exit('unexpected files in the download: %s' % bad)
"
}

if [ "$WINDOWS" = 1 ]; then
    DEPS="$HERE/build-pc-deps"
    SDL_ROOT="$DEPS/SDL2-$SDL_VERSION/x86_64-w64-mingw32"
    if [ ! -f "$SDL_ROOT/include/SDL2/SDL.h" ]; then
        say "SDL2 $SDL_VERSION for Windows (SDL's MinGW package)"
        mkdir -p "$DEPS"
        curl -fsSL -o "$DEPS/sdl2.tar.gz" "$SDL_URL"
        echo "$SDL_SHA256  $DEPS/sdl2.tar.gz" | sha256sum -c --quiet ||
            fail "SDL2's package does not match its checksum: $DEPS/sdl2.tar.gz"
        tar -xzf "$DEPS/sdl2.tar.gz" -C "$DEPS"
    fi
    say "Compiling for Windows"
    # The program's icon: SSR_PC_ICON, an .ico of your own (none comes with the source).
    ICON="${SSR_PC_ICON:-}"
    [ -n "$ICON" ] && { [ -f "$ICON" ] || fail "SSR_PC_ICON: no such file: $ICON"; ICON="$(realpath "$ICON")"; say "Icon: $ICON"; }
    cmake -S . -B build-pc-windows "${COMMON[@]}" -DCMAKE_TOOLCHAIN_FILE="$HERE/tools/pc/mingw-w64.cmake" \
        -DSDL2_DIR="$HERE/tools/pc/sdl2-mingw" -DSSR_SDL2_ROOT="$SDL_ROOT" -DSSR_PC_ICON="$ICON" >/dev/null
    ninja -C build-pc-windows ssrecomp
    OUT=build-pc-dist/windows/pokemon-ds-wide
    stage "$OUT"
    cp build-pc-windows/ssrecomp.exe "$OUT/Pokemon DS Launcher.exe"
    x86_64-w64-mingw32-strip "$OUT/Pokemon DS Launcher.exe"
    cp "$SDL_ROOT/bin/SDL2.dll" "$OUT/"
    cp "$DEPS/SDL2-$SDL_VERSION/LICENSE.txt" "$OUT/LICENSE-SDL2.txt"
    ZIP=pokemon-ds-wide-pc-windows-x86_64.zip
    rm -f "build-pc-dist/$ZIP"
    (cd build-pc-dist/windows && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk('pokemon-ds-wide'):
        dirs.sort()
        d = zipfile.ZipInfo(root + '/')
        d.external_attr = (0o40755 << 16) | 0x10  # a folder, for Unix and Windows unzippers
        z.writestr(d, '')
        for f in sorted(files): z.write(os.path.join(root, f))
" "../$ZIP")
    python3 -c "import sys, zipfile; print('\n'.join(zipfile.ZipFile(sys.argv[1]).namelist()))" "build-pc-dist/$ZIP" | check_contents
    say "Done: $HERE/build-pc-dist/$ZIP"
fi

if [ "$LINUX" = 1 ]; then
    # SDL2 comes with the download, built here from its source with its display and sound parts
    # (X11, Wayland, PulseAudio, ALSA) loaded only when used, so that it needs nothing but the C
    # library: SteamOS (the Steam Deck) and other systems without SDL2 run the download as it
    # is. The program looks for it beside itself first (RPATH $ORIGIN, see CMakeLists.txt).
    # (Ubuntu's own libSDL2 is linked to some twenty libraries, so it is not the one shipped.)
    LSDL="$HERE/build-pc-deps/SDL2-$SDL_VERSION-linux"
    if [ ! -f "$LSDL/build/libSDL2-2.0.so.0" ]; then
        say "SDL2 $SDL_VERSION for Linux, from its source"
        mkdir -p "$LSDL"
        curl -fsSL -o "$LSDL/src.tar.gz" "$SDL_SRC_URL"
        echo "$SDL_SRC_SHA256  $LSDL/src.tar.gz" | sha256sum -c --quiet ||
            fail "SDL2's source does not match its checksum: $LSDL/src.tar.gz"
        tar -xzf "$LSDL/src.tar.gz" -C "$LSDL"
        cmake -S "$LSDL/SDL2-$SDL_VERSION" -B "$LSDL/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF >/dev/null
        ninja -C "$LSDL/build" >/dev/null
    fi
    extra="$(readelf -d "$LSDL/build/libSDL2-2.0.so.0" | grep NEEDED | grep -v 'libc\.so\|libm\.so' || true)"
    [ -z "$extra" ] || fail "the SDL2 built for the download needs more than the C library: $extra"
    say "Compiling for Linux"
    cmake -S . -B build-pc-linux "${COMMON[@]}" >/dev/null
    ninja -C build-pc-linux ssrecomp
    OUT=build-pc-dist/linux/pokemon-ds-wide
    stage "$OUT"
    cp build-pc-linux/ssrecomp "$OUT/pokemon-ds-wide"
    strip "$OUT/pokemon-ds-wide"
    readelf -d "$OUT/pokemon-ds-wide" | grep -q 'R.*PATH.*\$ORIGIN' || fail "the Linux program does not look for SDL2 beside itself"
    cp "$LSDL/build/libSDL2-2.0.so.0" "$OUT/libSDL2-2.0.so.0"
    strip "$OUT/libSDL2-2.0.so.0"
    cp "$LSDL/SDL2-$SDL_VERSION/LICENSE.txt" "$OUT/LICENSE-SDL2.txt"
    TGZ=pokemon-ds-wide-pc-linux-x86_64.tar.gz
    tar -czf "build-pc-dist/$TGZ" -C build-pc-dist/linux pokemon-ds-wide
    tar -tzf "build-pc-dist/$TGZ" | check_contents
    say "Done: $HERE/build-pc-dist/$TGZ"
fi
if [ "$MAC" = 1 ]; then
    # The program in an .app, with the roms, saves and data folders beside the .app: SDL's
    # SDL_GetBasePath, from which the game list finds them, gives the .app's own folder
    # (SDL_FILESYSTEM_BASE_DIR_TYPE parent). SDL2 is built into the program
    # (cmake/MacDependencies.cmake). No OpenGL here: the processor draws the world.
    ARCH="$(uname -m)"
    say "Compiling for macOS ($ARCH)"
    cmake -S . -B build-pc-mac "${COMMON[@]}" -DSSR_GPU3D=OFF -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
    ninja -C build-pc-mac ssrecomp
    OUT=build-pc-dist/mac/pokemon-ds-wide
    stage "$OUT"
    APP="$OUT/Pokemon DS Launcher.app"
    mkdir -p "$APP/Contents/MacOS"
    cp build-pc-mac/ssrecomp "$APP/Contents/MacOS/pokemon-ds-wide"
    strip "$APP/Contents/MacOS/pokemon-ds-wide"
    cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>pokemon-ds-wide</string>
    <key>CFBundleIdentifier</key><string>io.github.mamesj.pokemonds</string>
    <key>CFBundleName</key><string>Pokemon DS Launcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>SDL_FILESYSTEM_BASE_DIR_TYPE</key><string>parent</string>
</dict>
</plist>
EOF
    # (Stripping undoes the linker's own signature, without which Apple silicon does not run a
    # program: the .app is signed again, ad hoc.)
    codesign --force --sign - "$APP"
    cp build-pc-mac/_deps/ssr_sdl2-src/LICENSE.txt "$OUT/LICENSE-SDL2.txt"
    ZIP=pokemon-ds-wide-pc-macos-$ARCH.zip
    rm -f "build-pc-dist/$ZIP"
    ditto -c -k --norsrc --noextattr --keepParent "$OUT" "build-pc-dist/$ZIP"
    python3 -c "import sys, zipfile; print('\n'.join(zipfile.ZipFile(sys.argv[1]).namelist()))" "build-pc-dist/$ZIP" | check_contents
    say "Done: $HERE/build-pc-dist/$ZIP"
fi
ls -l build-pc-dist/*.zip build-pc-dist/*.tar.gz 2>/dev/null
cat <<EOF

Unpack the download anywhere, put your ROMs in its roms folder (any names: English Black, White,
Black 2 and White 2; SoulSilver, HeartGold and Platinum US) and start Pokemon DS Launcher.exe
(Linux: pokemon-ds-wide; Mac: Pokemon DS Launcher.app).
The downloads hold only the program: nothing of the games.
EOF
