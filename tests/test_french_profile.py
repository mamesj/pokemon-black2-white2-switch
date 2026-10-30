"""Synthetic instruction and data fixtures, never commercial game content."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('match_functions', ROOT/'tools/regions/match_functions.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RegionalMatchingTests(unittest.TestCase):
    def test_unique_thumb_function_after_relocation(self):
        code = struct.pack('<8H', 0xb510, 0x2001, 0x2102, 0x2203, 0x2304, 0x2405, 0xbd10, 0x46c0)
        target = bytes(32) + code + bytes(64)
        self.assertEqual(module.unique_match(code, target, 0, 'thumb'), (32, 16))
        self.assertIsNone(module.unique_match(code, code+bytes(16)+code, 0, 'thumb'))

    def test_data_tables_are_unique_or_rejected(self):
        block = struct.pack('<8I', *range(1, 9))
        self.assertEqual(module.unique_data_address((0x2000000, block), (0x2000100, bytes(16)+block), 0x2000000, len(block)), 0x2000110)
        with self.assertRaisesRegex(ValueError, 'multiple'):
            module.unique_data_address((0x2000000, block), (0x2000100, block+block), 0x2000000, len(block))

    def test_black_white_tables_have_the_weathers(self):
        # main.cpp looks these up by names it puts together (bw2.weatherObj).
        for code in ('IRBF', 'IRAF', 'IREF', 'IRDF'):
            names = {line.split()[-1] for line in (ROOT/'tools/regions'/(code + '.txt')).read_text().splitlines()
                     if line and not line.startswith('#')}
            self.assertEqual(module.COMPOSED - names, set(), code)
