#!/usr/bin/env python3
"""Match named functions between two personally supplied DS ROMs.

Emits address metadata only. It never embeds instructions or ROM data. Branch
and literal-pool offsets are normalised with the project's existing matcher.
Ambiguous functions remain unresolved unless independently matched neighbours
on both sides agree on their displacement and the complete body matches.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import struct
import sys

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / 'bw1'))
from ndsrom import read_rom
from match_bw2 import tokens


def images(path):
    with open(path, 'rb') as rom:
        header = rom.read(0x200)
    main, overlays = read_rom(path)
    base = struct.unpack_from('<I', header, 0x28)[0]
    return header[12:16].decode('ascii'), {'main': (base, main),
            **{f'ov{oid}': value for oid, value in overlays.items()}}


def module_key(name):
    match = re.fullmatch(r'ov(\d+)', name)
    return 'ov' + str(int(match[1])) if match else name


def load_functions(path):
    modules = defaultdict(list)
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        fields = line.split()
        if len(fields) != 5 or fields[1] not in ('a', 't'):
            continue
        address, isa, module, _, name = fields
        modules[module_key(module)].append({'addr': int(address, 16),
            'isa': 'thumb' if isa == 't' else 'arm', 'name': name, 'module': module_key(module)})
    for funcs in modules.values():
        funcs.sort(key=lambda f: f['addr'])
    return modules


def unique_match(reference, target, guess, isa, window=1024):
    step = 2 if isa == 'thumb' else 4
    # A longer prefix removes lookalike wrappers; a shorter one may still be
    # unique where a language-dependent constant changes later in the body.
    for length in sorted({min(len(reference), n) // step * step for n in (96, 64, 32, 16)}, reverse=True):
        if length < 12:
            continue
        wanted = tokens(reference[:length], isa)
        hits = [off for off in range(max(0, guess-window)//step*step,
                                    min(len(target)-length, guess+window)+1, step)
                if tokens(target[off:off+length], isa) == wanted]
        if len(hits) == 1:
            return hits[0], length
    return None


MAP_DATA = {
    'IRBO': ('ov21', 0x021d3700, 0x0209d8b0, 21, 0x199500),
    'IRAO': ('ov21', 0x021d3718, 0x0209d8c8, 21, 0x199500),
    'IREO': ('ov36', 0x021ca01c, 0x0208ee68, 29, 0x1a3e00),
    'IRDO': ('ov36', 0x021ca050, 0x0208ee94, 29, 0x1a3e00),
}


def unique_data_address(reference, target, address, size):
    old_base, old_data = reference
    new_base, new_data = target
    offset = address-old_base
    if offset < 0 or offset+size > len(old_data):
        raise ValueError('Reference data outside its module')
    block = old_data[offset:offset+size]
    found, start = [], 0
    while (at := new_data.find(block, start)) >= 0:
        if at % 4 == 0:
            found.append(at)
        start = at+1
    if len(found) == 1:
        return new_base+found[0]
    if found:
        raise ValueError('Data table has multiple exact matches')
    wanted = tokens(block, 'arm')
    found = [at for at in range(max(0, offset-1024)//4*4,
                                min(len(new_data)-size, offset+1024)+1, 4)
             if tokens(new_data[at:at+size], 'arm') == wanted]
    if len(found) != 1:
        raise ValueError('Data table does not have a unique match')
    return new_base+found[0]


def match(reference_path, target_path, table_path, frontend):
    source_code, source = images(reference_path)
    target_code, target = images(target_path)
    modules = load_functions(table_path)
    literals = set()
    for path in frontend:
        literals.update(re.findall(r'"([A-Za-z_][A-Za-z_0-9]+)"', Path(path).read_text(encoding='utf-8')))
    literals -= {'input', 'main', 'update'}
    resolved, required, missing = [], [], []
    for module, funcs in modules.items():
        if module not in source or module not in target:
            continue
        source_base, old_data = source[module]
        target_base, new_data = target[module]
        wanted = {i for i, f in enumerate(funcs) if f['name'] in literals}
        indices = {j for i in wanted for j in range(max(0, i-8), min(len(funcs), i+9))}
        hits = {}
        for i in sorted(indices):
            f = funcs[i]
            end = funcs[i+1]['addr'] if i+1 < len(funcs) else source_base+len(old_data)
            offset = f['addr']-source_base
            if offset < 0 or end-source_base > len(old_data):
                continue
            reference = old_data[offset:end-source_base]
            found = unique_match(reference, new_data, offset, f['isa'])
            if found:
                hits[i] = (target_base+found[0], 'unique-prefix-' + str(found[1]))
        for i in sorted(wanted - hits.keys()):
            left = next((j for j in range(i-1, -1, -1) if j in hits), None)
            right = next((j for j in range(i+1, len(funcs)) if j in hits), None)
            if left is None or right is None:
                continue
            delta = hits[left][0]-funcs[left]['addr']
            if delta != hits[right][0]-funcs[right]['addr']:
                continue
            f = funcs[i]
            size = funcs[i+1]['addr']-f['addr'] if i+1<len(funcs) else 0
            old_offset, new_offset = f['addr']-source_base, f['addr']+delta-target_base
            if size < 4 or new_offset < 0 or new_offset+size > len(new_data):
                continue
            if tokens(old_data[old_offset:old_offset+size], f['isa']) == tokens(new_data[new_offset:new_offset+size], f['isa']):
                hits[i] = (f['addr']+delta, 'bounded-identical-body')
        for i in sorted(wanted):
            f = funcs[i]
            required.append(f['name'])
            if i in hits:
                address, evidence = hits[i]
                resolved.append({**f, 'target': address, 'evidence': evidence})
            else:
                missing.append(f)
    result = {'source_code': source_code, 'target_code': target_code,
              'functions': resolved, 'missing': missing}
    if source_code in MAP_DATA:
        module, table, heap, count, user = MAP_DATA[source_code]
        new_table = unique_data_address(source[module], target[module], table, count*72)
        new_heap = unique_data_address(source['main'], target['main'], heap, 12)
        result['wide_map'] = {'table': new_table, 'heap_setup': new_heap, 'count': count, 'heap_user': user}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference')
    parser.add_argument('target')
    parser.add_argument('table')
    parser.add_argument('out')
    args = parser.parse_args()
    root = TOOLS.parent
    frontend = [*sorted((root/'frontend').glob('*.h')), root/'frontend/main.cpp']
    result = match(args.reference, args.target, args.table, frontend)
    Path(args.out).write_text(json.dumps(result, indent=1)+'\n', encoding='utf-8', newline='\n')
    print(f'{result["source_code"]} -> {result["target_code"]}: {len(result["functions"])} functions matched; {len(result["missing"])} unresolved')
    for f in result['missing']:
        print(f'{f["module"]} {f["name"]} {f["addr"]:08x}')
    if result['missing']:
        raise SystemExit(1)
