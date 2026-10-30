# Notes for coding agents

This file is for AI coding agents (and people) working on ssrecomp. The README
is the user documentation for the games on the Switch; the original
project's README, `docs/original-ssrecomp-README.md`, has "How the recompiled
code works" and "Testing": read them before changing the recompiler.

## What this is

A static recompiler for Pokémon SoulSilver (US) built on the melonDS 1.1 core.
`tools/recomp.py` translates every ARM9 instruction of the game into C++
(`gen/`), which runs inside melonDS in place of its interpreter. The frontend
(SDL2) shows the game on **one screen**, merging the two DS screens, with a
widescreen view, 60 fps in-between frames and full controller support.
Targets: Linux and Windows first, Android second. macOS is community-maintained.

## Rules that must not be broken

- **Never commit, upload or paste game content:** `gen/` (the game's code in
  C++), ROMs (`*.nds`), `.nef`/`.sbin` build output, saves, save states
  (`*.st`, `*.state`), APKs, `build/`, `test/`. `.gitignore` covers them; don't
  force-add anything it ignores. Screenshots in `docs/screenshots` are fine.
- **Recompiled code must behave bit-identically to the interpreter.** After
  any change to `tools/recomp.py`, `tools/recomp_inline.py`, `runtime/` or the
  melonDS CPU code, regenerate `gen/`, rebuild, and pass `--lockstep` (below).
  A speed-up that breaks lockstep is a bug.
- **Measure performance claims** against `--interp` on the same snapshot, more
  than once (the timings vary by several percent with machine load). Many
  plausible optimisations were measured to be slower; see "Already tried".
- **Don't open pull requests to pret/pokeheartgold** (the decompilation) with
  AI-assisted code: pret does not accept it. Changes belong in this repository.
- **Don't change the release asset names** (`ssrecomp-setup-linux-x86_64`,
  `ssrecomp-setup-linux-aarch64.AppImage`, `ssrecomp-setup-windows-x86_64.exe`,
  `ssrecomp-setup-macos-arm64.zip`, `ssrecomp-setup-macos-x86_64.zip`,
  `SHA256SUMS.txt`): the setup program's updater looks for them.

## Layout

| Path | What |
|---|---|
| `tools/recomp.py` | the recompiler: reads the ROM (`--rom`, with `layouts/soulsilver-us.json`) or a decompilation build, writes `gen/rc_*.cpp` |
| `tools/recomp_inline.py` | inline C++ for common instructions, chosen by melonDS handler name; anything else calls the melonDS handler |
| `tools/apps.py` | writes `gen/apps_table.cpp`: each game application's per-frame function, so the frontend knows which screen (bag, battle, ...) is running |
| `runtime/recomp_gen.h` | per-instruction macros (fetch, guard, body, post) used by `gen/` |
| `runtime/recomp_rt.cpp` | chunk lookup, overlay byte checks, interpreter fallback |
| `runtime/recomp_mem.cpp` | out-of-line fast memory and jump helpers (out of line on purpose: inlining was slower) |
| `frontend/main.cpp` | SDL window, input, pause menu, options, test harness flags |
| `frontend/merged.h`, `layout.h` | choosing and composing the single-screen view |
| `frontend/battlewide.h` | the widescreen battle view (continues backgrounds and moves sprites beside the screen) |
| `frontend/gpucompose.h` | the same picture as `MergedCompose` in `merged.h`, drawn by the GPU (headless tests use the CPU one; keep both in step) |
| `frontend/ui.h`, `touchpad.h` | pause menu, touch cursor and messages; on-screen controls for Android |
| `frontend/platform.cpp`, `host.h` | melonDS's Platform interface (files, saves, threads) |
| `melonDS/` | melonDS 1.1 with this project's changes (dispatch into recompiled code, per-pixel layer flags, `GPU3D_Wide.cpp`, palette and blend snapshots for the widescreen view) |
| `melonDS-ssrecomp.patch` | the changes to existing melonDS files as a diff; update it when you change one |
| `ssrecomp_setup.py` | the setup program (Tk GUI and CLI): builds from the user's ROM, Android APK, self-update, portable mode |
| `setup.sh`, `run.sh` | building from the decompilation; starting the game (writes `game.log`) |
| `tools/package_setup.py`, `make_appimage.sh` | packaging the setup program with PyInstaller |
| `tools/android/` | APK packing around SDL's Java code |
| `frontend/switch_platform.cpp` | the Switch's parts (SD card folder, clocks, threads, exit); only file that sees libnx |
| `frontend/launcher.h` | the all-games program's game list (ROMs found, icons and names from their banners; ALL with a heading per console, and a tab for each: DS, GBA, 3DS), drawn by `switch_platform.cpp` and `pc_launcher.cpp` before the game starts |
| `frontend/gba_games.h` | GBA games in that list (`SSR_GBA_DIR`: Pokemon GBA Widescreen's folder, built in as the library `gbawide`; its `frontend/embed.h` finds and plays them), from the roms folder and a GBA folder in it |
| `frontend/bw1.h`, `tools/bw1/`, `tools/bw2/` | Pokémon Black, White and Black 2: no recompiled code; layouts follow the functions in `funcs.txt` (made by `funcmap.py` from a decompilation's symbols and the ROM), Black's own layouts on top of Black 2's (White's table is Black's at the addresses in `tools/bw1/white_shifts.txt`) |
| `melonDS/src/GPU3D_WideGpu*.cpp`, `wide_gpu/` | the widescreen world drawn by a graphics chip: deko3D on the Switch, OpenGL on a PC for headless comparisons (keep both in step with `GPU3D_Wide.cpp`) |
| `frontend/pc_launcher.cpp`, `tools/pc/` | the PC all-games download (Windows and Linux, `SSR_PC_ALL_GAMES`): the game list (launcher.h) when started without arguments, ROMs in `roms/`, saves in `saves/`, function tables in `data/funcs/`; `tools/pc/build_all.sh` builds and packs it (Windows with MinGW-w64 from Linux or WSL) |
| `tools/switch/` | Switch builds: `build_game.sh` (any of the games; `bw`: the shared Black 2/White 2 build with its tables in the romfs; `all`: one program for all seven with the game list), `build.sh` (SoulSilver) |
| `layouts/soulsilver-us.json` | section addresses and code/data map, so a plain ROM can be recompiled without the decompilation |

## Building

Needs CMake, Ninja, a C++17 compiler, SDL2, libpng, zlib, Python 3 (plus
`pyelftools` only when reading a decompilation build). On immutable Linux
(Bazzite, SteamOS, Silverblue) build inside a distrobox (Ubuntu 24.04).

From a ROM (what the setup program does):

    python3 tools/recomp.py --rom <rom.nds> --layout layouts/soulsilver-us.json melonDS/src gen
    python3 tools/apps.py --layout layouts/soulsilver-us.json gen
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ninja -C build

From the decompilation, see README "Building from the decompilation".

Costs to plan around:
- A full build compiles 1.7 million recompiled instructions: 20–30 minutes on
  6 cores and about 1.5 GB of RAM per job (`ninja -j8` on 16 GB).
- Changing only `frontend/` rebuilds one file in seconds:
  `ninja -C build ssrecomp`.
- Changing `tools/recomp*.py` or `runtime/recomp_gen.h` means regenerating
  `gen/` and recompiling all of it.

## Testing

You need your own ROM and snapshots; none are in the repository. Running
`build/ssrecomp` with no arguments lists every option.

- **Correctness:** `build/ssrecomp <rom> --lockstep --frames 3000 --input <script>`
  runs the interpreter and recompiled code side by side and stops at the first
  difference in registers, flags, cycles or timestamps. Cover booting and
  battles: e.g. `--state-load <overworld.st> --input <grass walk> --battle-mash --frames 7500`.
- **Speed:** `--headless --no-save --no-wide --state-load <snap> --frames 6880`,
  with and without `--interp`, compare the times.
- **Where the emulation's time goes:** `SSR_PERF=1 SSR_EMU_PROF=1` adds geometry, 2D (each
  screen), sound, DMA, the wide and in-between polygon lists, the 3D frames' starts and the
  waits for helper threads to the timing lines (`melonDS/src/SsrProf.h`); on the Switch,
  `arg --env SSR_EMU_PROF=1` in a test run's `test.txt` (the timing lines there also show the
  clocks and temperatures). On the Switch the GPU pictures are built, sent and fetched on two
  threads of their own (`WideGpuSend` in `GPU3D_WideGpu.cpp`); `SSR_GPU_SEND_THREAD=0` keeps
  that on the emulation thread, for comparing.
- **Pictures:** `--headless --compose 1280x720 --shot-dir DIR --shot-at a,b,c`
  (or `--shot-every 1` and ffmpeg for a video) shows the single-screen view.
  Check widescreen and battle changes visually, in motion: transitions, fades
  and the level-up window have all broken before.
- **Controllers without hardware:** input scripts accept `pad:a`, `pad:guide`,
  `pad:rx+`, etc. On Linux `/dev/uinput` can create a virtual pad for SDL.
- **Setup program:** `python3 ssrecomp_setup.py` (GUI) or `--rom FILE`
  (CLI build), `--update`.

## Conventions

- Match the surrounding code: C++17, 4-space indent, comments that explain
  *why* in full sentences, British spelling in text (colour, optimise).
- User-visible changes get a line under `## Unreleased` at the top of
  `CHANGELOG.md`. The setup program reads its own version from the first
  `## vX.Y.Z` heading, so don't add one until a release.
- README and changelog are for players: plain language, no internal names
  unless needed.
- Bug reports should come with `game.log` (or `build.log` for build problems);
  when adding diagnostics, print them to stderr so they land there.
- Keep the game playable from a controller alone, and keep the
  single-screen view the default.

## Already tried (measured, slower or no gain)

Chaining chunks inside the dispatch loop; inlining the memory/jump helpers
into every instruction (+30 MB of code, ~4% slower); a 4096-entry lookup cache
in front of chunk dispatch; building the melonDS core at -O3; keeping the
pipeline (CurInstr/NextInstr) or cycles/timestamp in chunk locals (spills,
up to 14% slower). The current recompiled code is about 6–9% faster than the
interpreter; the cost now is mostly melonDS's per-instruction bookkeeping that
lockstep requires.

Pairing an in-between frame's polygons (`GPU3D_Wide.cpp`, Pair): testing "moved more than an
eighth of the screen" with a multiplication before dividing, dividing once per candidate, and a
compact table of polygon kinds: same results, no faster on the Switch (in-between lists 1.59 ms
a frame before and after). Most of its time is the scene-motion vote's 140,000-odd candidate
tests a frame, not the divisions. What did help: building the wide lists and the pairs on a
helper thread (`ListHelper`, `SSR_WIDE_LISTS=0` to compare). Building the DS-resolution
in-between frame on that helper too (after the pairs) was slower on the Switch: the emulation
waited 2.4 ms for it at line 215 instead of 1.5 ms building it itself.

Not drawing the bottom screen when the view does not show it: the frontend reads its pixels
to choose what to show (AutoFocus, the battle and menu scenes), and in the overworld its lines
mostly come from the line cache already (0.1 ms on a PC). In a Black 2 battle the bottom screen
costs 2 ms on the Switch, but it is the battle menu.

Widescreen 3D (`GPU3D_Wide.cpp`): the pixel loop works on groups of 8 (AVX2)
or 4 (SSE, NEON) pixels; measure it with `SSR_WIDE_THREADS=1 SSR_WIDE_PROF=1`.
No gain: separate loop copies per kind of polygon without SIMD (2%), reading
texels without branches plus writing opaque groups as vectors (0%), keeping
decoded textures from one frame to the next.
