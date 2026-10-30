#!/usr/bin/env python3
"""Generate SoulSilver/HeartGold FR watch addresses from your own ROMs.

Only addresses are emitted. No instructions, game content or reference ROM
are retained in the profile. The reference must be the SoulSilver US revision
used by the checked-in layout; the target can be SoulSilver or HeartGold FR.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / 'regions'))
from match_functions import match
from ndsrom import read_rom


def core_addresses(layout):
    needed = {}
    for name, section, execute, init, init_section, exit_, exit_section in layout['apps']:
        for phase, sec, address in [('Main', section, execute), ('Init', init_section, init), ('Exit', exit_section, exit_)]:
            if address:
                needed[(sec, address)] = f'{name}_{phase}'
    for name, section, address in layout['focus']:
        needed[(section, address)] = name
    return needed


def generate(reference_path, target_path, layout_path):
    layout = json.loads(Path(layout_path).read_text(encoding='utf-8'))
    digest = hashlib.sha1()
    with open(reference_path, 'rb') as source:
        while block := source.read(4*1024*1024):
            digest.update(block)
    if digest.hexdigest() != layout['rom_sha1']:
        raise ValueError('The reference does not match the SoulSilver USA layout')
    with open(target_path, 'rb') as target:
        header = target.read(0x200)
    code = header[12:16].decode('ascii')
    if len(header) != 0x200 or code not in ('IPGF', 'IPKF') or header[0x1e] != 0:
        raise ValueError('Expected SoulSilver or HeartGold France, revision 0')
    needed = core_addresses(layout)
    rows = []
    sections = {}
    for section in layout['sections']:
        source = section['source']
        module = 'main' if source == 'main' else f'ov{source}' if isinstance(source, int) else 'itcm'
        sections[module] = section
        for start, end, thumb in section['ranges']:
            starts = {start, *[a for a in section['named'] if start < a < end],
                      *[a for sec, a in needed if sec == section['name'] and start <= a < end]}
            for address in sorted(starts):
                name = needed.get((section['name'], address), f'address_{address:08x}')
                rows.append(f'{address:08x} {"t" if thumb else "a"} {module} ? {name}')
    with tempfile.TemporaryDirectory() as tmp:
        table, names = Path(tmp)/'reference.txt', Path(tmp)/'names.txt'
        table.write_text('\n'.join(rows), encoding='utf-8')
        names.write_text('\n'.join(f'"{n}"' for n in needed.values()), encoding='utf-8')
        result = match(reference_path, target_path, table, [names])
    if result['missing']:
        raise ValueError('Unresolved probes: ' + ', '.join(f['name'] for f in result['missing']))
    _, overlays = read_rom(target_path)
    bases = {s['name']: overlays[s['source']][0] for s in layout['sections'] if isinstance(s['source'], int)}
    overrides = {}
    for function in result['functions']:
        section = sections[function['module']]
        overrides.setdefault(section['name'], {})[function['addr']] = function['target']-function['addr']
    shifts = {}
    for section in layout['sections']:
        name, start = section['name'], section['base']
        end = start+section['size']
        default = bases.get(name, start)-start
        changes = overrides.get(name, {})
        cuts = {start, end, *changes, *[a+2 for a in changes]}
        points = sorted(a for a in cuts if start <= a <= end)
        intervals = []
        for a, b in zip(points, points[1:]):
            delta = changes.get(a, default)
            if not delta:
                continue
            if intervals and intervals[-1][1] == a and intervals[-1][2] == delta:
                intervals[-1][1] = b
            else:
                intervals.append([a, b, delta])
        if intervals:
            shifts[name] = intervals
    title = 'SoulSilver' if code == 'IPGF' else 'HeartGold'
    print(f'{len(result["functions"])} {code} application/control probes matched; addresses only')
    return {'game': f'Pokemon {title} (France, revision 0)', 'code': code, 'like': 'IPGE',
            'overlay_bases': bases, 'shifts': shifts}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom')
    parser.add_argument('out')
    parser.add_argument('--reference', required=True)
    parser.add_argument('--layout', default=str(TOOLS.parent/'layouts/soulsilver-us.json'))
    args = parser.parse_args()
    profile = generate(args.reference, args.rom, args.layout)
    Path(args.out).write_text(json.dumps(profile, indent=1)+'\n', encoding='utf-8', newline='\n')
