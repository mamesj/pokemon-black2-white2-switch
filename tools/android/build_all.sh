#!/usr/bin/env bash
# Builds the Android all-games app: one APK for every game (Pokemon Black, White, Black 2,
# White 2, SoulSilver, HeartGold and Platinum) with the PC program's game list
# (frontend/pc_launcher.cpp). The first time it starts, the player picks the folder with their
# ROMs (Android's folder picker); the saves go in a saves folder inside it.
#
#   tools/android/build_all.sh your-soulsilver-us.nds
#
# As for the PC and Switch all-games downloads, the SoulSilver ROM is only read for the
# addresses in SoulSilver and HeartGold's tables, and the Black games' function tables come from
# the decompilations' symbol files: the APK holds nothing of the games.
#
# The result: build-android-dist/pokemon-ds-wide-android.apk, for arm64-v8a (phones, tablets and
# handhelds) and x86_64 (the Android emulator on a PC). SSR_ANDROID_ABIS="arm64-v8a" makes a
# phone-only one.
#
# Needs Linux or WSL2 (Ubuntu 24.04) with git, CMake, Ninja, Python 3, curl, zip, a JDK (17) and
# Pillow (python3-pil), and the Android SDK in $ANDROID_SDK (default ~/android-sdk) with
# build-tools;34.0.0, platforms;android-34 and ndk;27.2.12479018. SDL2's source is fetched once
# into build-android-deps/ and checked against its known checksum (the same SDL2 as the setup
# program's Android build). Options:
#   SSR_ANDROID_ICON=icon.png   the app's icon (any picture; default the setup program's)
#   SSR_PC_ART=folder           pictures for the game list, built into the app (IREO.jpg ...,
#                               named as for the PC download's icons/ folder)
#   SSR_ANDROID_KEY=key.json    the signing key, made on the first build: keep it, as Android
#                               only installs an update over an app signed with the same key
#                               (default ~/.config/pokemon-ds-wide/android-signing-key.json)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$HERE"
ROM="${1:-}"
SDK="${ANDROID_SDK:-$HOME/android-sdk}"
NDK="$SDK/ndk/27.2.12479018"
BT="$SDK/build-tools/34.0.0"
JAR="$SDK/platforms/android-34/android.jar"
ABIS="${SSR_ANDROID_ABIS:-arm64-v8a x86_64}"
KEY="${SSR_ANDROID_KEY:-$HOME/.config/pokemon-ds-wide/android-signing-key.json}"
ICON="${SSR_ANDROID_ICON:-$HERE/tools/appimage/ssrecomp-setup.png}"
ART="${SSR_PC_ART:-}"
BW2_DECOMP=https://github.com/fuddlesworth/pokebw2.git
BW2_COMMIT=753b3fd97683fc619bb98b52e7276afac3d5e32f
W2_COMMIT=be2c125a700c23702ba1c5a4ef63e0e970bcc87a
BW1_DECOMP=https://github.com/Project-Holon/aletheia.git
BW1_COMMIT=0d05b6ccc61fcea090d8318a00af9f1634810e18
SDL_NAME=SDL2-2.32.10
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/$SDL_NAME.tar.gz"
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
PLATFORM=android-24   # Android 7.0 and later

say() { printf '\n== %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }

[ -n "$ROM" ] || fail "give your SoulSilver ROM (US): tools/android/build_all.sh your-soulsilver-us.nds"
[ -f "$ROM" ] || fail "no such file: $ROM"
[ "$(dd if="$ROM" bs=1 skip=12 count=4 2>/dev/null)" = IPGE ] ||
    fail "$ROM is not Pokemon SoulSilver (US, game code IPGE): the tables are made from it"
ROM="$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")"
for tool in git cmake ninja python3 curl zip javac; do
    command -v "$tool" >/dev/null || fail "$tool is not installed (Ubuntu: sudo apt install git cmake ninja-build python3 curl zip openjdk-17-jdk-headless)"
done
python3 -c 'import PIL' 2>/dev/null || fail "Pillow is not installed (Ubuntu: sudo apt install python3-pil)"
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || fail "the NDK is not in $NDK (sdkmanager --install 'ndk;27.2.12479018')"
[ -x "$BT/aapt2" ] || fail "build-tools 34.0.0 are not in $SDK (sdkmanager --install 'build-tools;34.0.0')"
[ -f "$JAR" ] || fail "platform 34 is not in $SDK (sdkmanager --install 'platforms;android-34')"
[ -f "$ICON" ] || fail "SSR_ANDROID_ICON: no such file: $ICON"
[ -z "$ART" ] || [ -d "$ART" ] || fail "SSR_PC_ART: no such folder: $ART"
[ -z "$ART" ] || ART="$(realpath "$ART")"

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

# SoulSilver and HeartGold's tables (addresses only) and the function tables, as for the PC
# download (tools/pc/build_all.sh).
if [ ! -f gen/sources.cmake ]; then
    say "SoulSilver's tables, from $ROM"
    python3 tools/recomp.py --rom "$ROM" --layout layouts/soulsilver-us.json melonDS/src gen
    python3 tools/apps.py --layout layouts/soulsilver-us.json gen
fi
python3 tools/soulsilver/tables_only.py gen build-gen-tables
FUNCS="$HERE/build-android-data/funcs"
rm -rf "$FUNCS"
mkdir -p "$FUNCS"
say "The function tables, from the decompilations' symbol files"
fetch_symbols build-decomp-pokebw2 "$BW2_DECOMP" "$BW2_COMMIT" \
    '/config/b2_us/arm9/symbols.txt' '/config/b2_us/arm9/*/symbols.txt' '/config/b2_us/arm9/overlays/*/symbols.txt'
fetch_symbols build-decomp-pokew2 "$BW2_DECOMP" "$W2_COMMIT" \
    '/config/w2_us/arm9/symbols.txt' '/config/w2_us/arm9/*/symbols.txt' '/config/w2_us/arm9/overlays/*/symbols.txt'
fetch_symbols build-decomp-aletheia "$BW1_DECOMP" "$BW1_COMMIT" '/layout/function_addrs.tsv'
python3 tools/bw2/funcmap.py build-decomp-pokebw2/config/b2_us/arm9 - "$FUNCS/IREO.txt"
python3 tools/bw2/funcmap.py build-decomp-pokew2/config/w2_us/arm9 - "$FUNCS/IRDO.txt"
python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$FUNCS/IRBO.txt"
python3 tools/bw1/funcmap.py build-decomp-aletheia/layout/function_addrs.tsv - "$FUNCS/IRAO.txt" --white
cp tools/platinum/CPUE.txt "$FUNCS/CPUE.txt"
cp tools/regions/{IREF,IRDF,CPUF,IRBF,IRAF}.txt "$FUNCS/"

DEPS="$HERE/build-android-deps"
if [ ! -f "$DEPS/$SDL_NAME/CMakeLists.txt" ]; then
    say "$SDL_NAME's source"
    mkdir -p "$DEPS"
    curl -fsSL -o "$DEPS/$SDL_NAME.tar.gz" "$SDL_URL"
    echo "$SDL_SHA256  $DEPS/$SDL_NAME.tar.gz" | sha256sum -c --quiet ||
        fail "SDL2's source does not match its checksum: $DEPS/$SDL_NAME.tar.gz"
    tar -xzf "$DEPS/$SDL_NAME.tar.gz" -C "$DEPS"
fi

STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
FILES=()
for abi in $ABIS; do
    ARGS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
          -DANDROID_ABI="$abi" -DANDROID_PLATFORM="$PLATFORM")
    SDL_PREFIX="$DEPS/sdl2-$abi"
    if [ ! -f "$SDL_PREFIX/lib/libSDL2.so" ]; then
        say "SDL2 for Android ($abi)"
        cmake -S "$DEPS/$SDL_NAME" -B "$DEPS/build-sdl2-$abi" "${ARGS[@]}" -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
            -DSDL_STATIC=OFF -DSDL_TEST=OFF >/dev/null
        ninja -C "$DEPS/build-sdl2-$abi" install >/dev/null
    fi
    say "Compiling for Android ($abi)"
    # The JIT for every game, no libpng (screenshots are for tests), the world drawn by the CPU
    # (the graphics-chip version is OpenGL, Android has OpenGL ES), and on x86_64 SSE rather than
    # the building machine's AVX2.
    cmake -S . -B "build-android-all-$abi" "${ARGS[@]}" -DSDL2_DIR="$SDL_PREFIX/lib/cmake/SDL2" \
        -DSSR_JIT=ON -DSSR_PC_ALL_GAMES=ON -DSSR_PNG=OFF -DSSR_GPU3D=OFF -DSSR_PORTABLE_X86=ON \
        -DSSR_GENERATED_DIR="$HERE/build-gen-tables" -DSSR_PC_ART="$ART" >/dev/null
    ninja -C "build-android-all-$abi" ssrecomp
    mkdir -p "build-android-dist/lib/$abi"
    "$STRIP" -o "build-android-dist/lib/$abi/libmain.so" "build-android-all-$abi/libmain.so"
    "$STRIP" -o "build-android-dist/lib/$abi/libSDL2.so" "$SDL_PREFIX/lib/libSDL2.so"
    FILES+=("lib/$abi/libSDL2.so=build-android-dist/lib/$abi/libSDL2.so" "lib/$abi/libmain.so=build-android-dist/lib/$abi/libmain.so")
done
for f in "$FUNCS"/*.txt; do FILES+=("assets/funcs/$(basename "$f")=$f"); done

say "The app around it: manifest, icon, SDL's Java code and the app's own"
W="$HERE/build-android-dist/skeleton"
rm -rf "$W"
mkdir -p "$W/classes" "$W/dex"
# The icon: the picture as it is for Android 7 and older, and an adaptive icon for Android 8 on
# (mipmap-anydpi-v26), whose launchers cut it to their own shape (a circle on a Pixel). Without
# one, the square picture was shrunk inside a white circle. Its one layer is the picture at 70% of
# the layer's width with its edges carried outwards, so the shape is filled and the middle of the
# picture is what shows (SSR_ANDROID_ICON_FILL to change the 70%).
for d in mdpi:48:108 hdpi:72:162 xhdpi:96:216 xxhdpi:144:324 xxxhdpi:192:432; do
    IFS=: read -r dens legacy layer <<< "$d"
    mkdir -p "$W/res/mipmap-$dens"
    python3 - "$ICON" "$W/res/mipmap-$dens" "$legacy" "$layer" "${SSR_ANDROID_ICON_FILL:-0.7}" <<'PY'
import sys
from PIL import Image
src, out, legacy, layer, fill = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
im = Image.open(src).convert('RGB')
side = min(im.size)   # the middle square of the picture
im = im.crop(((im.width - side) // 2, (im.height - side) // 2, (im.width + side) // 2, (im.height + side) // 2))
im.resize((legacy, legacy), Image.LANCZOS).save(out + '/ic_launcher.png', optimize=True)
s = max(1, round(layer * fill))
# (Its outer 4% left out: a picture's own rounded corners or border would be carried outwards.)
i = side * 4 // 100
pic = im.crop((i, i, side - i, side - i)).resize((s, s), Image.LANCZOS)
o = (layer - s) // 2
bg = Image.new('RGB', (layer, layer))
# The edges carried outwards: each row's end pixels to the sides, then the top and bottom rows.
for y in range(s):
    bg.paste(pic.getpixel((0, y)), (0, o + y, o, o + y + 1))
    bg.paste(pic.getpixel((s - 1, y)), (o + s, o + y, layer, o + y + 1))
bg.paste(pic, (o, o))
top, bottom = bg.crop((0, o, layer, o + 1)), bg.crop((0, o + s - 1, layer, o + s))
for y in range(o):
    bg.paste(top, (0, y))
for y in range(o + s, layer):
    bg.paste(bottom, (0, y))
bg.save(out + '/ic_launcher_background.png', optimize=True)
PY
done
mkdir -p "$W/res/mipmap-anydpi-v26"
cat > "$W/res/mipmap-anydpi-v26/ic_launcher.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@mipmap/ic_launcher_background" />
    <foreground android:drawable="@android:color/transparent" />
</adaptive-icon>
XML
"$BT/aapt2" compile --dir "$W/res" -o "$W/res.zip"
"$BT/aapt2" link -o "$W/base.apk" -I "$JAR" --manifest tools/android/allgames/AndroidManifest.xml \
    --min-sdk-version 24 --target-sdk-version 34 "$W/res.zip"
javac -nowarn --release 11 -cp "$JAR" -d "$W/classes" \
    $(find "$DEPS/$SDL_NAME/android-project/app/src/main/java" tools/android/allgames/java -name '*.java') 2>&1 | grep -v '^Note:' || true
[ -f "$W/classes/io/github/mamesj/pokemonds/GameActivity.class" ] || fail "the app's Java code did not compile"
"$BT/d8" --release --min-api 24 --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')
(cd "$W/dex" && zip -q -X "$W/base.apk" classes.dex)

say "Packing and signing the APK"
APK="$HERE/build-android-dist/pokemon-ds-wide-android.apk"
python3 - "$W/base.apk" "$KEY" "$APK" "${FILES[@]}" <<'PY'
import sys
sys.path.insert(0, 'tools/android')
import apk
skeleton, key, out = sys.argv[1:4]
files = [tuple(a.split('=', 1)) for a in sys.argv[4:]]
apk.build_files(skeleton, files, key, out)
PY
# Only the app's own files may be in it: never a ROM, save or game code.
python3 - "$APK" <<'PY'
import re, sys, zipfile
ok = re.compile(r'^(AndroidManifest\.xml|resources\.arsc|classes\.dex|res/.*\.png|res/mipmap-anydpi-v26/ic_launcher\.xml|lib/(arm64-v8a|x86_64)/lib(main|SDL2)\.so|assets/funcs/(IREO|IRDO|IRBO|IRAO|CPUE|IREF|IRDF|CPUF|IRBF|IRAF)\.txt)$')
bad = [n for n in zipfile.ZipFile(sys.argv[1]).namelist() if not ok.match(n)]
if bad: sys.exit('unexpected files in the APK: %s' % bad)
PY
if [ -x "$BT/apksigner" ]; then "$BT/apksigner" verify "$APK" && echo "signature: ok"; fi
ls -l "$APK"
say "Done: $APK"
cat <<EOF
Install it with adb install -r "$APK", or copy it to the phone and open it (allow installing
from that app when Android asks). On the first start, tap the screen, allow access to files, and
pick the folder with your ROMs.
EOF
