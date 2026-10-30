#!/usr/bin/env bash
# Builds one of the games for the Nintendo Switch from your own ROM, all steps in one:
#
#   tools/switch/build_game.sh soulsilver your-soulsilver-us.nds
#   tools/switch/build_game.sh black2     your-black2-us.nds
#   tools/switch/build_game.sh white2     your-white2-us.nds
#   tools/switch/build_game.sh bw
#   tools/switch/build_game.sh hgss       your-soulsilver-us.nds
#   tools/switch/build_game.sh white      your-white-us-or-europe.nds
#   tools/switch/build_game.sh all        your-soulsilver-us.nds
#   tools/switch/build_game.sh platinum
#
# bw needs no ROM: one program for Black 2, White 2, Black and White that reads what it needs
# from the player's ROM when it starts, packed as build-sd/pokemon-black2-white2-wide-switch.zip and
# build-sd/pokemon-black-white-wide-switch.zip to share (the releases' downloads). hgss likewise packs one program for SoulSilver and HeartGold
# (US) as build-sd/pokemon-soulsilver-heartgold-wide-switch.zip: the SoulSilver ROM is only read for
# the addresses in its tables (tools/soulsilver/tables_only.py); none of its code goes in.
#   tools/switch/build_game.sh black      your-black-us-or-europe.nds
#
# platinum needs no ROM either: one program for Pokemon Platinum (English) with its function table
# inside (tools/platinum/CPUE.txt, made by tools/platinum/funcmap.py from a build of the
# pokeplatinum decompilation: addresses and names only), packed as
# build-sd/pokemon-platinum-wide-switch.zip.
#
# all packs one program for all the games, with a list to choose the game from when it starts
# (frontend/launcher.h): SoulSilver and HeartGold's tables (as hgss) and the Black games'
# function tables (as bw) in one .nro, as build-sd/pokemon-ds-wide-switch.zip
# (switch/pokemon-ds-wide/pokemon-ds-wide.nro), as build-sd/pokemon-ds-wide-all-switch.zip: the
# all-games release's download. It holds nothing of the games either.
# SSR_SWITCH_ICON_ALL: its Homebrew Menu icon.
#
# The result is a folder laid out like the SD card, build-sd/switch/<game folder>/, with the
# .nro (and, for Black, Black 2 and White 2, funcs.txt): copy build-sd/switch to the root of the SD card,
# then your ROM into the game's folder (see README, "Nintendo Switch").
#
# Needs Linux or WSL2 (Ubuntu 24.04) with git, CMake, Ninja and Python 3, and devkitPro with
# switch-dev and switch-sdl2. When devkitPro is not installed but Docker is, its official image
# (devkitpro/devkita64) is used instead: their servers refuse some IP addresses (403).
# SSR_SWITCH_ICON=path/to/icon.jpg (256x256 JPEG) gives the game your own Homebrew Menu icon
# (for bw: SSR_SWITCH_ICON_BLACK2, SSR_SWITCH_ICON_WHITE2, SSR_SWITCH_ICON_BLACK and
# SSR_SWITCH_ICON_WHITE, one each; for hgss:
# SSR_SWITCH_ICON_SOULSILVER and SSR_SWITCH_ICON_HEARTGOLD).
#
# Keep funcs.txt (black2, white2, black, white) to yourself: it is made from your ROM. Never share a
# build of the recompiled code (tools/switch/build.sh without SSR_SWITCH_JIT=1, build-switch/):
# it is the game's code.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
GAME="${1:-}"
ROM="${2:-}"
IMAGE=devkitpro/devkita64
# The decompilation versions the function names come from (only their symbol files are fetched).
BW2_DECOMP=https://github.com/fuddlesworth/pokebw2.git
BW2_COMMIT=753b3fd97683fc619bb98b52e7276afac3d5e32f
# (White 2's symbols came later. Its functions have the same names as Black 2's.)
W2_COMMIT=be2c125a700c23702ba1c5a4ef63e0e970bcc87a
BW1_DECOMP=https://github.com/Project-Holon/aletheia.git
BW1_COMMIT=0d05b6ccc61fcea090d8318a00af9f1634810e18

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

case "$GAME" in
    soulsilver) FOLDER=ssrecomp; NRO=ssrecomp.nro ;;
    black2) FOLDER=pokemon-black2-wide; NRO=pokemon-black2-wide.nro ;;
    white2) FOLDER=pokemon-white2-wide; NRO=pokemon-white2-wide.nro ;;
    black) FOLDER=pokemon-black-wide; NRO=pokemon-black-wide.nro ;;
    white) FOLDER=pokemon-white-wide; NRO=pokemon-white-wide.nro ;;
    bw|hgss|all|platinum) FOLDER=; NRO= ;;
    *) fail "which game? tools/switch/build_game.sh soulsilver|hgss|all|black2|white2|black|white your-rom.nds (or bw or platinum, no ROM)" ;;
esac
if [ "$GAME" != bw ] && [ "$GAME" != platinum ]; then
    [ -n "$ROM" ] || fail "give your ROM: tools/switch/build_game.sh $GAME your-rom.nds"
    [ -f "$ROM" ] || fail "no such file: $ROM"
    ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"
fi
for tool in git cmake ninja python3; do
    command -v "$tool" >/dev/null || fail "$tool is not installed (Ubuntu: sudo apt install git cmake ninja-build python3)"
done

# The toolchain: devkitPro installed here, or else its Docker image.
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DOCKER=""
if [ -f "$DEVKITPRO/cmake/Switch.cmake" ] && [ -f "$DEVKITPRO/portlibs/switch/lib/libSDL2.a" ]; then
    export DEVKITPRO
    export PATH="$DEVKITPRO/tools/bin:$PATH"
elif command -v docker >/dev/null 2>&1; then
    if docker info >/dev/null 2>&1; then DOCKER=docker; else DOCKER="sudo docker"; fi
    say "devkitPro is not installed here: using its Docker image ($IMAGE, about 2 GB the first time)"
    $DOCKER pull "$IMAGE"
else
    fail "devkitPro's Switch toolchain is not in $DEVKITPRO. Install devkitPro, then: sudo dkp-pacman -S switch-dev switch-sdl2 (or install Docker)"
fi
# Runs a build script with the toolchain (the source folder is mounted at the same path in the
# container, so a build folder's paths stay valid between runs).
build() {
    if [ -z "$DOCKER" ]; then
        env "$@"
    else
        local envs=()
        while [ "$#" -gt 0 ] && [[ "$1" == *=* ]]; do envs+=(-e "$1"); shift; done
        $DOCKER run --rm -v "$HERE:$HERE" -w "$HERE" "${envs[@]}" "$IMAGE" "$@"
        $DOCKER run --rm -v "$HERE:$HERE" "$IMAGE" chown -R "$(id -u):$(id -g)" "$HERE/$BUILD"
    fi
}

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

# The Black games' function tables, made without a ROM (the program reads the few bytes the tables
# need from the player's ROM), into a romfs folder: bw and all.
bw_tables() { # romfs-folder
    mkdir -p "$1/funcs"
    say "The function tables, from the decompilations' symbol files"
    fetch_symbols build-decomp-pokebw2 "$BW2_DECOMP" "$BW2_COMMIT" \
        '/config/b2_us/arm9/symbols.txt' '/config/b2_us/arm9/*/symbols.txt' '/config/b2_us/arm9/overlays/*/symbols.txt'
    fetch_symbols build-decomp-pokew2 "$BW2_DECOMP" "$W2_COMMIT" \
        '/config/w2_us/arm9/symbols.txt' '/config/w2_us/arm9/*/symbols.txt' '/config/w2_us/arm9/overlays/*/symbols.txt'
    fetch_symbols build-decomp-aletheia "$BW1_DECOMP" "$BW1_COMMIT" '/layout/function_addrs.tsv'
    python3 tools/bw2/funcmap.py build-decomp-pokebw2/config/b2_us/arm9 - "$1/funcs/IREO.txt"
    python3 tools/bw2/funcmap.py build-decomp-pokew2/config/w2_us/arm9 - "$1/funcs/IRDO.txt"
    python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$1/funcs/IRBO.txt"
    python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$1/funcs/IRAO.txt" --white
}

case "$GAME" in
all)
    # One program for all six games: SoulSilver and HeartGold's tables (addresses only, made from
    # the SoulSilver ROM as for hgss) and the Black games' function tables in its romfs (as for bw).
    # It shows the list of the player's games when it starts (frontend/launcher.h).
    BUILD=build-switch-all
    [ "$(dd if="$ROM" bs=1 skip=12 count=4 2>/dev/null)" = IPGE ] ||
        fail "$ROM is not Pokemon SoulSilver (US, game code IPGE): the tables are made from it"
    if [ ! -f gen/sources.cmake ]; then
        say "SoulSilver's tables, from $ROM"
        python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
        python3 tools/apps.py --layout layouts/soulsilver-us.json gen
    fi
    python3 tools/soulsilver/tables_only.py gen build-gen-tables
    ROMFS="$HERE/$BUILD/romfs"
    bw_tables "$ROMFS"
    cp tools/platinum/CPUE.txt "$ROMFS/funcs/CPUE.txt"
    cp tools/regions/{IREF,IRDF,CPUF,IRBF,IRAF}.txt "$ROMFS/funcs/"
    # The HOME Menu entries' program (HOME MENU ICONS in the list's settings: frontend/switch_forwarder.cpp).
    say "The HOME Menu icons' loader (tools/switch/forwarder)"
    build bash tools/switch/build_forwarder.sh "$HERE/$BUILD/forwarder-build" "$ROMFS/forwarder"
    [ -s "$ROMFS/forwarder/main" ] && [ -s "$ROMFS/forwarder/main.npdm" ] || fail "the HOME Menu icons' loader was not made"
    # The list's box art built in (SSR_PC_ART: a folder of your own, IREO.jpg ..., as for the PC
    # download), copied beside the build so that the build (in Docker too) can see it.
    ART=""
    if [ -n "${SSR_PC_ART:-}" ]; then
        [ -d "$SSR_PC_ART" ] || fail "SSR_PC_ART: no such folder: $SSR_PC_ART"
        rm -rf "$BUILD/box-art"
        mkdir -p "$BUILD/box-art"
        cp "$SSR_PC_ART"/*.jpg "$SSR_PC_ART"/*.png "$BUILD/box-art/" 2>/dev/null || true
        ART="$HERE/$BUILD/box-art"
        say "Pictures for the game list: $SSR_PC_ART"
    fi
    icon="${SSR_SWITCH_ICON_ALL:-${SSR_SWITCH_ICON:-}}"
    rm -f "$BUILD/icon.jpg"
    [ -n "$icon" ] && install -D -m 644 "$icon" "$BUILD/icon.jpg"
    # GBA games too (SSR_GBA_DIR: Pokemon GBA Widescreen's folder, after its setup.sh): with
    # devkitPro installed or unpacked (DEVKITPRO), not its Docker image, which sees only this folder.
    if [ -n "${SSR_GBA_DIR:-}" ]; then
        [ -f "$SSR_GBA_DIR/frontend/embed.h" ] || fail "SSR_GBA_DIR: not Pokemon GBA Widescreen: $SSR_GBA_DIR"
        [ -z "$DOCKER" ] || fail "GBA games too (SSR_GBA_DIR) needs devkitPro installed, not its Docker image: set DEVKITPRO (Pokemon GBA Widescreen's tools/switch/build.sh unpacks one)"
        SSR_GBA_DIR="$(realpath "$SSR_GBA_DIR")"
        export SSR_GBA_DIR
        say "GBA games too: $SSR_GBA_DIR"
    fi
    say "Compiling for the Switch"
    build SSR_SWITCH_JIT=1 SSR_SWITCH_GEN="$HERE/build-gen-tables" SSR_SWITCH_BUILD="$BUILD" \
        SSR_SWITCH_ROMFS="$ROMFS" SSR_ALL_GAMES=ON SSR_PC_ART="$ART" SSR_NRO_TITLE="Pokémon Fulldex" bash tools/switch/build.sh
    mkdir -p build-sd/switch/pokemon-ds-wide/roms
    cp "$BUILD/ssrecomp.nro" build-sd/switch/pokemon-ds-wide/pokemon-ds-wide.nro
    # The licences: this program's (GPL-3) and, with the GBA games built in, mGBA's (MPL-2.0).
    cp LICENSE build-sd/switch/pokemon-ds-wide/LICENSE.txt
    LICENCES="switch/pokemon-ds-wide/LICENSE.txt"
    GBA_WORK="${GBAW_WORK:-$HOME/pokemon-gba-wide}"
    if [ -n "${SSR_GBA_DIR:-}" ]; then
        [ -f "$GBA_WORK/mgba/LICENSE" ] || fail "no mGBA in $GBA_WORK (Pokemon GBA Widescreen's setup.sh makes it; GBAW_WORK if elsewhere)"
        cp "$GBA_WORK/mgba/LICENSE" build-sd/switch/pokemon-ds-wide/LICENSE-mGBA.txt
        LICENCES="$LICENCES switch/pokemon-ds-wide/LICENSE-mGBA.txt"
    else
        rm -f build-sd/switch/pokemon-ds-wide/LICENSE-mGBA.txt
    fi
    ZIP=pokemon-ds-wide-all-switch.zip
    rm -f "build-sd/$ZIP"
    # The program, its licences and an empty roms folder, so that it is plain where the ROMs go.
    (cd build-sd && python3 -c "
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('switch/pokemon-ds-wide/pokemon-ds-wide.nro')
    for f in sys.argv[2:]: z.write(f)
    d = zipfile.ZipInfo('switch/pokemon-ds-wide/roms/')
    d.external_attr = (0o40755 << 16) | 0x10  # a folder, for Unix and Windows unzippers
    z.writestr(d, '')
" "$ZIP" $LICENCES)
    # (Only the program and its licences: no ROM, save, save state or generated code.)
    python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
want = ['switch/pokemon-ds-wide/pokemon-ds-wide.nro'] + sys.argv[2:] + ['switch/pokemon-ds-wide/roms/']
if names != want: sys.exit('unexpected files in the zip: %s' % names)
print('zip holds:', ', '.join(names))
" "build-sd/$ZIP" $LICENCES
    say "Done: $HERE/build-sd/$ZIP"
    ls -l build-sd/switch/pokemon-ds-wide "build-sd/$ZIP"
    cat <<EOF

Copy build-sd/switch to the root of the SD card, then your ROMs into /switch/pokemon-ds-wide/roms/ (any
names; English Black, White, Black 2, White 2; SoulSilver and HeartGold US$([ -n "${SSR_GBA_DIR:-}" ] && printf '; the GBA games, Emerald,\nFireRed, LeafGreen and Ultra Violet, there or in a GBA folder in it')). Its list also shows
the games in the other folders of /switch (the one-game downloads), each with the save in its folder.
On the Switch, hold R while starting any game and choose Pokémon Fulldex.
The zip holds only the program, ready to share: nothing of the games.
EOF
    exit 0
    ;;
soulsilver)
    BUILD=build-switch-jit
    if [ ! -f gen/sources.cmake ]; then
        say "The game's tables, from $ROM"
        python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
        python3 tools/apps.py --layout layouts/soulsilver-us.json gen
    fi
    # Without the recompiled code: the game runs on melonDS's JIT, about 1.7 times as fast on the
    # Switch, and the build takes minutes instead of most of an hour.
    python3 tools/soulsilver/tables_only.py gen build-gen-tables
    [ -n "${SSR_SWITCH_ICON:-}" ] && install -D -m 644 "$SSR_SWITCH_ICON" "$BUILD/icon.jpg"
    say "Compiling for the Switch"
    build SSR_SWITCH_JIT=1 SSR_SWITCH_GEN="$HERE/build-gen-tables" SSR_SWITCH_BUILD="$BUILD" bash tools/switch/build.sh
    ;;
black2|white2)
    # (Both games are the same program: White 2 reuses Black 2's build folder, so building the
    # second takes only seconds.)
    BUILD=build-switch-bw2
    if [ "$GAME" = black2 ]; then
        NAME="Black 2"; CFG=b2_us; DECOMP_DIR=build-decomp-pokebw2; COMMIT="$BW2_COMMIT"
    else
        NAME="White 2"; CFG=w2_us; DECOMP_DIR=build-decomp-pokew2; COMMIT="$W2_COMMIT"
    fi
    say "$NAME's function table, from the pokebw2 decompilation's symbol files and your ROM"
    fetch_symbols "$DECOMP_DIR" "$BW2_DECOMP" "$COMMIT" \
        "/config/$CFG/arm9/symbols.txt" "/config/$CFG/arm9/*/symbols.txt" "/config/$CFG/arm9/overlays/*/symbols.txt"
    mkdir -p "$BUILD"
    python3 tools/bw2/funcmap.py "$DECOMP_DIR/config/$CFG/arm9" "$ROM" "$BUILD/funcs.txt"
    [ -n "${SSR_SWITCH_ICON:-}" ] && install -D -m 644 "$SSR_SWITCH_ICON" "$BUILD/icon.jpg"
    say "Compiling for the Switch"
    build SSR_SWITCH_BUILD="$BUILD" SSR_NRO_TITLE="Pokemon $NAME" SSR_SWITCH_ROMFS= bash tools/bw2/build_switch.sh
    ;;
bw)
    # One program for Black 2, White 2, Black and White, with their function tables inside (made
    # without a ROM: the program reads the few bytes the tables need from the player's ROM).
    # Nothing of the games is in it, so it can be shared (a release's download).
    BUILD=build-switch-bwshared
    ROMFS="$HERE/$BUILD/romfs"
    bw_tables "$ROMFS"
    say "Compiling for the Switch"
    # Packed once per game, with its own title and icon in the Homebrew Menu (the same program).
    # The icons: SSR_SWITCH_ICON_BLACK2, SSR_SWITCH_ICON_WHITE2, SSR_SWITCH_ICON_BLACK and
    # SSR_SWITCH_ICON_WHITE (256x256 JPEG), else SSR_SWITCH_ICON for all, else libnx's.
    for g in black2:"Black 2" white2:"White 2" black:"Black" white:"White"; do
        var="SSR_SWITCH_ICON_$(echo "${g%%:*}" | tr a-z A-Z)"
        icon="${!var:-${SSR_SWITCH_ICON:-}}"
        rm -f "$BUILD/icon.jpg"
        [ -n "$icon" ] && install -D -m 644 "$icon" "$BUILD/icon.jpg"
        build SSR_SWITCH_BUILD="$BUILD" SSR_NRO_TITLE="Pokemon ${g#*:}" SSR_SWITCH_ROMFS="$ROMFS" bash tools/bw2/build_switch.sh
        mkdir -p "build-sd/switch/pokemon-${g%%:*}-wide"
        cp "$BUILD/ssrecomp.nro" "build-sd/switch/pokemon-${g%%:*}-wide/pokemon-${g%%:*}-wide.nro"
    done
    rm -f build-sd/pokemon-black2-white2-wide-switch.zip
    (cd build-sd && python3 -c "
import sys, zipfile
with zipfile.ZipFile('pokemon-black2-white2-wide-switch.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for p in sys.argv[1:]: z.write(p)
" switch/pokemon-black2-wide/pokemon-black2-wide.nro switch/pokemon-white2-wide/pokemon-white2-wide.nro)
    # Black and White's own download (their own release): the same program, a folder each.
    rm -f build-sd/pokemon-black-wide-switch.zip build-sd/pokemon-black-white-wide-switch.zip
    (cd build-sd && python3 -c "
import sys, zipfile
with zipfile.ZipFile('pokemon-black-white-wide-switch.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for p in sys.argv[1:]: z.write(p)
" switch/pokemon-black-wide/pokemon-black-wide.nro switch/pokemon-white-wide/pokemon-white-wide.nro)
    # (Only the programs: no ROM, save, save state or function table made from a ROM.)
    for zip in pokemon-black2-white2-wide-switch.zip pokemon-black-white-wide-switch.zip; do
        python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
bad = [n for n in names if not n.endswith('.nro')]
if bad: sys.exit('unexpected files in the zip: %s' % names)
print(sys.argv[1], 'holds:', ', '.join(names))
" "build-sd/$zip"
    done
    say "Done: $HERE/build-sd/pokemon-black2-white2-wide-switch.zip and pokemon-black-white-wide-switch.zip"
    ls -l build-sd/switch/pokemon-black2-wide build-sd/switch/pokemon-white2-wide build-sd/switch/pokemon-black-wide \
        build-sd/switch/pokemon-white-wide build-sd/pokemon-black2-white2-wide-switch.zip build-sd/pokemon-black-white-wide-switch.zip
    cat <<EOF

Copy build-sd/switch to the root of the SD card, then a Black 2 ROM into /switch/pokemon-black2-wide/,
a White 2 ROM into /switch/pokemon-white2-wide/, a Black ROM into /switch/pokemon-black-wide/ and a
White ROM into /switch/pokemon-white-wide/ (English, US or Europe; any copy of each works).
On the Switch, hold R while starting any game and choose it in the Homebrew Menu.
The zips hold only these programs, ready to share: nothing of the games.
EOF
    exit 0
    ;;
platinum)
    # One program for Pokemon Platinum (English), its function table in its romfs (addresses and
    # names, made without a ROM: the program reads the few bytes the table needs from the
    # player's ROM). Nothing of the game is in it, so it can be shared.
    BUILD=build-switch-platinum
    ROMFS="$HERE/$BUILD/romfs"
    mkdir -p "$ROMFS/funcs"
    cp tools/platinum/CPUE.txt "$ROMFS/funcs/CPUE.txt"
    cp tools/regions/CPUF.txt "$ROMFS/funcs/"
    rm -f "$BUILD/icon.jpg"
    icon="${SSR_SWITCH_ICON_PLATINUM:-${SSR_SWITCH_ICON:-}}"
    [ -n "$icon" ] && install -D -m 644 "$icon" "$BUILD/icon.jpg"
    say "Compiling for the Switch"
    build SSR_SWITCH_BUILD="$BUILD" SSR_NRO_TITLE="Pokemon Platinum" SSR_SWITCH_ROMFS="$ROMFS" bash tools/bw2/build_switch.sh
    mkdir -p build-sd/switch/pokemon-platinum-wide
    cp "$BUILD/ssrecomp.nro" build-sd/switch/pokemon-platinum-wide/pokemon-platinum-wide.nro
    ZIP=pokemon-platinum-wide-switch.zip
    rm -f "build-sd/$ZIP"
    (cd build-sd && python3 -c "
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('switch/pokemon-platinum-wide/pokemon-platinum-wide.nro')
" "$ZIP")
    # (Only the program: no ROM, save or save state.)
    python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
if names != ['switch/pokemon-platinum-wide/pokemon-platinum-wide.nro']: sys.exit('unexpected files in the zip: %s' % names)
print('zip holds:', ', '.join(names))
" "build-sd/$ZIP"
    say "Done: $HERE/build-sd/$ZIP"
    cat <<EOF

Copy build-sd/switch to the root of the SD card, then a Platinum ROM (English) into
/switch/pokemon-platinum-wide/. On the Switch, hold R while starting any game and choose it in the
Homebrew Menu. The zip holds only the program, ready to share: nothing of the game.
EOF
    exit 0
    ;;
hgss)
    # One program for SoulSilver and HeartGold (US) on melonDS's JIT, from the tables only
    # (addresses: tools/soulsilver/tables_only.py); the overlay code its watch compares is read
    # from the player's ROM when the game starts. Nothing of the games is in it, so it can be
    # shared (a release's download).
    BUILD=build-switch-jit
    [ "$(dd if="$ROM" bs=1 skip=12 count=4 2>/dev/null)" = IPGE ] ||
        fail "$ROM is not Pokemon SoulSilver (US, game code IPGE): the tables are made from it"
    if [ ! -f gen/sources.cmake ]; then
        say "The game's tables, from $ROM"
        python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
        python3 tools/apps.py --layout layouts/soulsilver-us.json gen
    fi
    python3 tools/soulsilver/tables_only.py gen build-gen-tables
    say "Compiling for the Switch"
    # Packed once per game, with its own title and icon in the Homebrew Menu (the same program).
    for g in soulsilver:SoulSilver heartgold:HeartGold; do
        var="SSR_SWITCH_ICON_$(echo "${g%%:*}" | tr a-z A-Z)"
        icon="${!var:-${SSR_SWITCH_ICON:-}}"
        rm -f "$BUILD/icon.jpg"
        [ -n "$icon" ] && install -D -m 644 "$icon" "$BUILD/icon.jpg"
        build SSR_SWITCH_JIT=1 SSR_SWITCH_GEN="$HERE/build-gen-tables" SSR_SWITCH_BUILD="$BUILD" \
            SSR_NRO_TITLE="Pokemon ${g#*:}" bash tools/switch/build.sh
        mkdir -p "build-sd/switch/pokemon-${g%%:*}-wide"
        cp "$BUILD/ssrecomp.nro" "build-sd/switch/pokemon-${g%%:*}-wide/pokemon-${g%%:*}-wide.nro"
    done
    ZIP=pokemon-soulsilver-heartgold-wide-switch.zip
    rm -f "build-sd/$ZIP"
    (cd build-sd && python3 -c "
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for p in sys.argv[2:]: z.write(p)
" "$ZIP" switch/pokemon-soulsilver-wide/pokemon-soulsilver-wide.nro switch/pokemon-heartgold-wide/pokemon-heartgold-wide.nro)
    # (Only the two programs: no ROM, save, save state or generated code.)
    python3 -c "
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
bad = [n for n in names if not n.endswith('.nro')]
if bad or len(names) != 2: sys.exit('unexpected files in the zip: %s' % names)
print('zip holds:', ', '.join(names))
" "build-sd/$ZIP"
    say "Done: $HERE/build-sd/$ZIP"
    ls -l build-sd/switch/pokemon-soulsilver-wide build-sd/switch/pokemon-heartgold-wide "build-sd/$ZIP"
    cat <<EOF

Copy build-sd/switch to the root of the SD card, then a SoulSilver ROM into
/switch/pokemon-soulsilver-wide/ and a HeartGold ROM into /switch/pokemon-heartgold-wide/ (English, US).
On the Switch, hold R while starting any game and choose it in the Homebrew Menu.
The zip is the same two files, ready to share: it holds nothing of the games.
EOF
    exit 0
    ;;
black)
    BUILD=build-switch-bw1
    say "Black's function table, from the aletheia decompilation's function list and your ROM"
    fetch_symbols build-decomp-aletheia "$BW1_DECOMP" "$BW1_COMMIT" '/layout/function_addrs.tsv'
    mkdir -p "$BUILD"
    python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv "$ROM" "$BUILD/funcs.txt"
    [ -n "${SSR_SWITCH_ICON:-}" ] && install -D -m 644 "$SSR_SWITCH_ICON" "$BUILD/icon.jpg"
    say "Compiling for the Switch"
    build bash tools/bw1/build_switch.sh
    ;;
white)
    # The same program as Black's, with White's table: Black's functions at White's addresses.
    BUILD=build-switch-bw1
    say "White's function table, from the aletheia decompilation's function list and your ROM"
    fetch_symbols build-decomp-aletheia "$BW1_DECOMP" "$BW1_COMMIT" '/layout/function_addrs.tsv'
    mkdir -p "$BUILD"
    python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv "$ROM" "$BUILD/funcs.txt" --white
    [ -n "${SSR_SWITCH_ICON:-}" ] && install -D -m 644 "$SSR_SWITCH_ICON" "$BUILD/icon.jpg"
    say "Compiling for the Switch"
    build SSR_NRO_TITLE="Pokemon White" bash tools/bw1/build_switch.sh
    ;;
esac

OUT="build-sd/switch/$FOLDER"
mkdir -p "$OUT"
cp "$BUILD/ssrecomp.nro" "$OUT/$NRO"
[ -f "$BUILD/funcs.txt" ] && cp "$BUILD/funcs.txt" "$OUT/funcs.txt"
say "Done: $HERE/$OUT"
ls -l "$OUT"
cat <<EOF

Copy build-sd/switch to the root of the SD card, then your ROM into /switch/$FOLDER/
$( [ "$GAME" = soulsilver ] && echo "(renamed rom.nds)" || echo "(as the only .nds file there)" ). On the Switch, hold R while starting any game
and choose it in the Homebrew Menu.
EOF
