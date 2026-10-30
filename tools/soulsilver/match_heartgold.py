#!/usr/bin/env python3
"""Where SoulSilver's functions are in HeartGold (US, or another language): a developer tool, run
once per game; its output, tools/soulsilver/heartgold-<region>.json, holds addresses only and is
what the build uses.

    python3 tools/soulsilver/match_heartgold.py <soulsilver.nds> <heartgold.nds> <gen dir> OUT.json [TITLE]

TITLE is the game's name as the program reports it (default: Pokemon HeartGold (USA)); the game
code comes from the ROM.

HeartGold's code is SoulSilver's with a few changes: 115 of the 121 code sections are the same
size, most byte for byte. Where code moved (the Pokedex, the main menu's Pal Park and Ranger
options, the title screen), each of SoulSilver's functions (the chunks in <gen dir>/rc_tables.cpp)
is looked for near its own address in HeartGold, by its instructions with branch targets and
literal-pool offsets masked. The result is, per section, the stretches where HeartGold's
address is SoulSilver's plus a shift: {"section": [[from, to, shift], ...]} (SoulSilver
addresses, from inclusive, to exclusive). Functions that are not found keep the shift of the
one before them.

The Italian HeartGold's main program is 96 bytes shorter (its data, not its code), so every
overlay is loaded 0x60 or 0x120 bytes lower, and its Pokedex's code drifts by up to 1000 bytes:
the overlays' load addresses are taken from each ROM, and functions are looked for up to 2 KB
away.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from ndsrom import read_rom  # noqa: E402
from recomp import autoload_image  # noqa: E402

LAYOUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'layouts', 'soulsilver-us.json')


def mask_thumb(b):
    out = []
    for i in range(0, len(b) - 1, 2):
        x = b[i] | b[i + 1] << 8
        if (x & 0xF800) in (0xF000, 0xF800, 0xE800):
            out.append(0xF000)                 # BL / BLX halves
        elif (x & 0xF800) == 0x4800:
            out.append(x & 0xFF00)             # LDR Rd, [PC, #imm]
        elif (x & 0xF000) == 0xD000:
            out.append(x & 0xFF00)             # conditional branch
        elif (x & 0xF800) == 0xE000:
            out.append(0xE000)                 # branch
        else:
            out.append(x)
    return out


def mask_arm(b):
    out = []
    for i in range(0, len(b) - 3, 4):
        x = int.from_bytes(b[i:i + 4], 'little')
        if (x & 0x0E000000) == 0x0A000000:
            out.append(x & 0xFF000000)         # B / BL / BLX
        elif (x & 0x0F7F0000) == 0x051F0000:
            out.append(x & 0xFFFFF000)         # LDR Rd, [PC, #imm]
        else:
            out.append(x)
    return out


def chunks_of(gen):
    """(start, end, thumb, section index) of every chunk, and the sections' names."""
    text = open(os.path.join(gen, 'rc_tables.cpp')).read()
    chunks = [(int(a, 16), int(b, 16), int(t), int(s)) for a, b, t, s in
              re.findall(r'\{0x([0-9A-F]+)u, 0x([0-9A-F]+)u, (\d), (\d+), [^}]*\}', text)]
    names = re.findall(r'\{"(\w+)", 0x[0-9A-F]+u, 0x[0-9A-F]+u, ', text)
    return chunks, names


def main():
    ss_rom, hg_rom, gen, out = sys.argv[1:5]
    title = sys.argv[5] if len(sys.argv) > 5 else 'Pokemon HeartGold (USA)'
    with open(hg_rom, 'rb') as f:
        code = f.read(0x10)[0x0C:].decode('ascii')
    layout = json.load(open(LAYOUT))
    ss_main, ss_ovl = read_rom(ss_rom)
    hg_main, hg_ovl = read_rom(hg_rom)
    chunks, names = chunks_of(gen)
    result = {}
    for e in layout['sections']:
        base = e['base']
        if e['source'] in ('main', 'itcm'):
            a, b = ss_main, hg_main
            if e['source'] == 'itcm':
                a, b = autoload_image(a, base), autoload_image(b, base)
            # (Same size: the same code at the same addresses; the bytes that differ are data.
            # HeartGold US; the Italian one's is shorter, which moves the overlays too.)
            if len(a) == len(b):
                continue
            hg_base = base
        else:
            hg_base, b = hg_ovl[int(e['source'])]
            a = ss_ovl[int(e['source'])][1]
            if a == b and hg_base == base:
                continue
        # An overlay loaded elsewhere: its functions' offsets in it are looked for as usual, and
        # the difference in where it is loaded is added to their shifts.
        moved = hg_base - base
        sec = names.index(e['name'])
        segs = []
        shift, found, missed = 0, 0, 0
        for start, end, thumb, s in chunks:
            if s != sec:
                continue
            off = start - base
            if e['source'] == 'main' and off < 0x800:
                continue                       # the secure area: each game's own, not code
            n = min(end - start, 24)
            mask = mask_thumb if thumb else mask_arm
            want = mask(a[off:off + n])
            best = None
            for d in sorted(range(-2048, 2049, 2 if thumb else 4), key=lambda d: (abs(d - shift), abs(d))):
                o = off + d
                if o < 0 or o + n > len(b):
                    continue
                if mask(b[o:o + n]) == want:
                    best = d
                    break
            if best is None:
                missed += 1
            else:
                found += 1
                shift = best
            if segs and segs[-1][2] == shift + moved:
                segs[-1][1] = end
            else:
                segs.append([start, end, shift + moved])
        # Stretches that need no shift are left out.
        segs = [s for s in segs if s[2]]
        if segs:
            result[e['name']] = segs
        print(f"{e['name']}: {found} functions found, {missed} not, {len(segs)} shifted stretches", file=sys.stderr)
    json.dump({'game': title, 'code': code, 'like': 'IPGE', 'shifts': result}, open(out, 'w'), indent=1)


if __name__ == '__main__':
    main()
