#!/usr/bin/env bash
# Builds the Mac all-games app, Pokemon Fulldex.app (the same program as the PC download,
# frontend/pc_launcher.cpp), on a Mac, for its own processor (Apple silicon or Intel), zipped as
# build-mac-dist/pokemon-ds-wide-pc-macos-<arm64|x86_64>.zip.
#
#   tools/mac/build_all.sh TABLES [ART] [ICON]
#
# TABLES: SoulSilver and HeartGold's tables (addresses only: tools/soulsilver/tables_only.py, the
# build-gen-tables folder that tools/pc/build_all.sh makes from the builder's ROM), so that no ROM
# is needed here. ART: the game list's pictures (as SSR_PC_ART); ICON: the app's icon (a square
# JPEG or PNG). The function tables come from the decompilations' symbol files, as on a PC.
#
# The app keeps the ROMs, saves and settings in ~/Library/Application Support/Pokemon DS
# Widescreen (O in the game list opens its roms folder). It is not notarised: the first start is
# right click > Open. A Mac's OpenGL stops at 4.1, so the widescreen world is drawn by the
# processor (no WORLD ON GPU). Needs Xcode's command line tools, CMake (3.21 to 3.x), Ninja,
# Python 3 and git (.github/workflows/mac.yml has them).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
TABLES="$(cd "${1:?which tables folder? tools/mac/build_all.sh TABLES [ART] [ICON]}" && pwd)"
ART="${2:-}"; [ -n "$ART" ] && ART="$(cd "$ART" && pwd)"
ICON="${3:-}"
# The processor to build for: this Mac's own, or SSR_MAC_ARCH (arm64 or x86_64; an Intel Mac
# builds for Apple silicon too, which the workflow uses when GitHub has no Apple silicon Mac free).
ARCH="${SSR_MAC_ARCH:-$(uname -m)}"
BW2_DECOMP=https://github.com/fuddlesworth/pokebw2.git
BW2_COMMIT=753b3fd97683fc619bb98b52e7276afac3d5e32f
W2_COMMIT=be2c125a700c23702ba1c5a4ef63e0e970bcc87a
BW1_DECOMP=https://github.com/Project-Holon/aletheia.git
BW1_COMMIT=0d05b6ccc61fcea090d8318a00af9f1634810e18

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\n== FAILED: %s\n' "$*" >&2; exit 1; }
[ -f "$TABLES/sources.cmake" ] || fail "no tables in $TABLES (tools/soulsilver/tables_only.py makes them)"
# (Only addresses: the recompiled code and the game's code are never in a tables folder.)
ls "$TABLES"/rc_[0-9]*.cpp >/dev/null 2>&1 && fail "$TABLES holds recompiled game code: use tables_only.py's output"

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
DATA="$HERE/build-mac-data"
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
cp tools/regions/{IREF,IRDF,CPUF,IRBF,IRAF}.txt "$DATA/funcs/"

say "Compiling for the Mac ($ARCH)"
# (SSR_PORTABLE_X86: no AVX2 for an Intel Mac without it, and no x86 test when building for
# Apple silicon on an Intel Mac. melonDS takes its JIT, ARM64 or x86-64, from the compiler's target.)
OPTS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DSSR_JIT=ON -DSSR_PC_ALL_GAMES=ON -DSSR_PNG=OFF -DSSR_GPU3D=OFF
      -DSSR_GENERATED_DIR="$TABLES" -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DSSR_PORTABLE_X86=ON)
[ -n "$ART" ] && OPTS+=(-DSSR_PC_ART="$ART")
cmake -S . -B build-mac "${OPTS[@]}" >/dev/null
ninja -C build-mac ssrecomp

say "The app"
DIST="$HERE/build-mac-dist"
APP="$DIST/Pokemon Fulldex.app"
rm -rf "$DIST"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/data"
cp build-mac/ssrecomp "$APP/Contents/MacOS/pokemon-ds-wide"
strip -x "$APP/Contents/MacOS/pokemon-ds-wide"
cp -r "$DATA/funcs" "$APP/Contents/Resources/data/funcs"
cp tools/pc/README.txt "$APP/Contents/Resources/README.txt"
cp LICENSE "$APP/Contents/Resources/LICENSE.txt"
ICONKEY=""
if [ -n "$ICON" ]; then
    SET="$DIST/AppIcon.iconset"
    mkdir -p "$SET"
    SQ="$DIST/icon-1024.png"
    sips -s format png -z 1024 1024 "$ICON" --out "$SQ" >/dev/null
    for s in 16 32 128 256 512; do
        sips -z $s $s "$SQ" --out "$SET/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$SQ" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$SET" -o "$APP/Contents/Resources/AppIcon.icns"
    rm -rf "$SET" "$SQ"
    ICONKEY="<key>CFBundleIconFile</key><string>AppIcon</string>"
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Pokémon Fulldex</string>
<key>CFBundleDisplayName</key><string>Pokémon Fulldex</string>
<key>CFBundleIdentifier</key><string>io.github.mamesj.pokemonds</string>
<key>CFBundleExecutable</key><string>pokemon-ds-wide</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
$ICONKEY
<key>LSMinimumSystemVersion</key><string>11.0</string>
<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
<key>NSHighResolutionCapable</key><true/>
<key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict></plist>
PLIST
lipo -archs "$APP/Contents/MacOS/pokemon-ds-wide" | grep -qx "$ARCH" || fail "the program is not built for $ARCH"
python3 tools/macos/check_binary.py "$APP/Contents/MacOS/pokemon-ds-wide" --max-macos 11.0 --system-libraries-only
# Signed for this Mac only ("ad hoc"): Apple silicon starts nothing unsigned. Not notarised.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"

# Started once without a window to check that it runs and finds its folders (on a Mac of its kind).
if [ "$ARCH" = "$(uname -m)" ]; then
say "A first start (no window)"
SUPPORT="$HOME/Library/Application Support/Pokemon DS Widescreen"
rm -f "$SUPPORT/game.log"
( SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$APP/Contents/MacOS/pokemon-ds-wide" >/dev/null 2>&1 & pid=$!; sleep 8; kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true )
[ -f "$SUPPORT/game.log" ] && head -5 "$SUPPORT/game.log"
grep -q "\[launcher\] folder $SUPPORT" "$SUPPORT/game.log" || fail "the app did not start as it should (see above)"
[ -d "$SUPPORT/roms" ] || fail "the app made no roms folder"
else
    say "(Built for $ARCH on a $(uname -m) Mac: not started here)"
fi

ZIP="$DIST/pokemon-ds-wide-pc-macos-$ARCH.zip"
(cd "$DIST" && ditto -c -k --sequesterRsrc --keepParent "Pokemon Fulldex.app" "$ZIP")
# Only the program, its tables, notes, licence and icon: never a ROM, save or game code.
python3 - "$ZIP" <<'PY'
import re, sys, zipfile
ok = re.compile(r'^Pokemon Fulldex\.app/(Contents/(Info\.plist|MacOS/(pokemon-ds-wide)?|_CodeSignature/(CodeResources)?|Resources/(AppIcon\.icns|README\.txt|LICENSE\.txt|data/|data/funcs/|data/funcs/(IREO|IRDO|IRBO|IRAO|CPUE|IREF|IRDF|CPUF|IRBF|IRAF)\.txt)?)?)?$')
names = zipfile.ZipFile(sys.argv[1]).namelist()
bad = [n for n in names if not ok.match(n)]
if bad: sys.exit('unexpected files in the Mac download: %s' % bad)
print('ok: %s (%d entries)' % (sys.argv[1].split('/')[-1], len(names)))
PY
say "Done: $ZIP"
