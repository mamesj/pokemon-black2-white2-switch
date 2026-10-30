#!/usr/bin/env bash
# Builds the iPhone and iPad app, Pokemon DS Widescreen (the same program as the PC download,
# frontend/pc_launcher.cpp), on a Mac with Xcode, as an unsigned .ipa for sideloading
# (AltStore, SideStore, Sideloadly and the like sign it with the player's own Apple ID):
# build-ios-dist/pokemon-ds-wide-ios.ipa.
#
#   tools/ios/build_all.sh TABLES [ART] [ICON]
#
# TABLES, ART and ICON as for tools/mac/build_all.sh (SoulSilver and HeartGold's tables, made
# without a ROM here; the game list's pictures; the app's icon). SSR_IOS_TARGET=simulator builds
# for the iOS Simulator instead (build-ios-dist/Pokemon DS Widescreen.app, for a first-start test:
# .github/workflows/ios.yml).
#
# The app keeps the ROMs, saves, settings and game.log in its Documents folder, which the Files
# app shows as On My iPhone > Pokemon DS Widescreen. iOS lets no app make code as it runs, so
# melonDS's JIT is left out and its interpreter plays (a recent iPhone is fast enough). The world
# is drawn by the processor, as on Android. Needs Xcode, CMake (3.21 to 3.x), Ninja, Python 3, git.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
TABLES="$(cd "${1:?which tables folder? tools/ios/build_all.sh TABLES [ART] [ICON]}" && pwd)"
ART="${2:-}"; [ -n "$ART" ] && ART="$(cd "$ART" && pwd)"
ICON="${3:-}"
TARGET="${SSR_IOS_TARGET:-device}"
BW2_DECOMP=https://github.com/fuddlesworth/pokebw2.git
BW2_COMMIT=753b3fd97683fc619bb98b52e7276afac3d5e32f
W2_COMMIT=be2c125a700c23702ba1c5a4ef63e0e970bcc87a
BW1_DECOMP=https://github.com/Project-Holon/aletheia.git
BW1_COMMIT=0d05b6ccc61fcea090d8318a00af9f1634810e18
MIN_IOS=14.0

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
DATA="$HERE/build-ios-data"
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

if [ "$TARGET" = simulator ]; then
    SDK=iphonesimulator
    ARCH="${SSR_IOS_ARCH:-$(uname -m)}"
else
    SDK=iphoneos
    ARCH=arm64
fi
say "Compiling for iOS ($SDK, $ARCH)"
BUILD="build-ios-$TARGET"
OPTS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK"
      -DCMAKE_OSX_ARCHITECTURES="$ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS"
      -DSSR_JIT=OFF -DSSR_PC_ALL_GAMES=ON -DSSR_PNG=OFF -DSSR_GPU3D=OFF -DSSR_GENERATED_DIR="$TABLES")
[ -n "$ART" ] && OPTS+=(-DSSR_PC_ART="$ART")
cmake -S . -B "$BUILD" "${OPTS[@]}" >/dev/null
ninja -C "$BUILD" ssrecomp

say "The app"
DIST="$HERE/build-ios-dist"
APP="$DIST/Pokemon DS Widescreen.app"
rm -rf "$DIST"
mkdir -p "$APP/data"
# (CMake makes an iOS program a bundle of its own, ssrecomp.app; only the program is taken from it.)
BIN="$BUILD/ssrecomp"
[ -f "$BUILD/ssrecomp.app/ssrecomp" ] && BIN="$BUILD/ssrecomp.app/ssrecomp"
cp "$BIN" "$APP/pokemon-ds-wide"
strip -x "$APP/pokemon-ds-wide"
cp -r "$DATA/funcs" "$APP/data/funcs"
cp LICENSE "$APP/LICENSE.txt"
ICONS=""
if [ -n "$ICON" ]; then
    SQ="$DIST/icon-1024.png"
    sips -s format png -z 1024 1024 "$ICON" --out "$SQ" >/dev/null
    # (iPhone 60 pt at 2x and 3x, iPad 76 pt at 1x and 2x and 83.5 pt at 2x.)
    sips -z 120 120 "$SQ" --out "$APP/AppIcon60x60@2x.png" >/dev/null
    sips -z 180 180 "$SQ" --out "$APP/AppIcon60x60@3x.png" >/dev/null
    sips -z 76 76 "$SQ" --out "$APP/AppIcon76x76~ipad.png" >/dev/null
    sips -z 152 152 "$SQ" --out "$APP/AppIcon76x76@2x~ipad.png" >/dev/null
    sips -z 167 167 "$SQ" --out "$APP/AppIcon83.5x83.5@2x~ipad.png" >/dev/null
    rm -f "$SQ"
    ICONS="<key>CFBundleIcons</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon60x60</string></array><key>CFBundleIconName</key><string>AppIcon</string></dict></dict>
<key>CFBundleIcons~ipad</key><dict><key>CFBundlePrimaryIcon</key><dict><key>CFBundleIconFiles</key><array><string>AppIcon60x60</string><string>AppIcon76x76</string><string>AppIcon83.5x83.5</string></array><key>CFBundleIconName</key><string>AppIcon</string></dict></dict>"
fi
PLATFORM=iPhoneOS; [ "$SDK" = iphonesimulator ] && PLATFORM=iPhoneSimulator
cat > "$APP/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>Pokemon DS Widescreen</string>
<key>CFBundleDisplayName</key><string>Pokemon DS</string>
<key>CFBundleIdentifier</key><string>io.github.mamesj.pokemonds</string>
<key>CFBundleExecutable</key><string>pokemon-ds-wide</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleSupportedPlatforms</key><array><string>$PLATFORM</string></array>
<key>MinimumOSVersion</key><string>$MIN_IOS</string>
<key>UIDeviceFamily</key><array><integer>1</integer><integer>2</integer></array>
<key>UIRequiredDeviceCapabilities</key><array><string>arm64</string></array>
<key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
<key>UISupportedInterfaceOrientations~ipad</key><array><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
<key>UIRequiresFullScreen</key><true/>
<key>UIStatusBarHidden</key><true/>
<key>UIViewControllerBasedStatusBarAppearance</key><false/>
<key>UILaunchScreen</key><dict/>
<key>UIFileSharingEnabled</key><true/>
<key>LSSupportsOpeningDocumentsInPlace</key><true/>
<key>UIApplicationSupportsIndirectInputEvents</key><true/>
<key>GCSupportsControllerUserInteraction</key><true/>
<key>GCSupportedGameControllers</key><array><dict><key>ProfileName</key><string>ExtendedGamepad</string></dict></array>
<key>ITSAppUsesNonExemptEncryption</key><false/>
$ICONS
</dict></plist>
PLIST
plutil -lint "$APP/Info.plist"
lipo -archs "$APP/pokemon-ds-wide" | grep -qw "$ARCH" || fail "the program is not built for $ARCH"
# Signed "ad hoc" (no Apple ID): the Simulator wants it; a sideloading tool signs the .ipa again
# with the player's own.
codesign --force --sign - --timestamp=none "$APP"

if [ "$TARGET" = simulator ]; then
    say "Done: $APP (for the iOS Simulator)"
    exit 0
fi
IPA="$DIST/pokemon-ds-wide-ios.ipa"
mkdir -p "$DIST/Payload"
cp -R "$APP" "$DIST/Payload/"
(cd "$DIST" && zip -qry "$IPA" Payload)
rm -rf "$DIST/Payload"
# Only the program, its tables, licence, icons and Info.plist: never a ROM, save or game code.
python3 - "$IPA" <<'PY'
import re, sys, zipfile
ok = re.compile(r'^Payload/(Pokemon DS Widescreen\.app/(pokemon-ds-wide|Info\.plist|LICENSE\.txt|AppIcon[0-9.x@~a-z]+\.png|_CodeSignature/(CodeResources)?|data/|data/funcs/|data/funcs/(IREO|IRDO|IRBO|IRAO|CPUE|IREF|IRDF|CPUF|IRBF|IRAF)\.txt)?)?$')
names = zipfile.ZipFile(sys.argv[1]).namelist()
bad = [n for n in names if not ok.match(n)]
if bad: sys.exit('unexpected files in the iOS download: %s' % bad)
print('ok: %s (%d entries)' % (sys.argv[1].split('/')[-1], len(names)))
PY
say "Done: $IPA"
