"""Address-only portable table regression fixtures."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PortableFrenchTests(unittest.TestCase):
    def test_portable_tables_keep_bios_sections(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            src = tmp/'source'
            src.mkdir()
            (src/'apps_table.cpp').write_text('// Synthetic application metadata\n')
            (src/'rc_tables.cpp').write_text('const unsigned rc_num_chunks = 0;\nRcSection rc_sections[] = {\n{"main", 0x02000000u, 0x20u, nullptr, nullptr, 0, 0, 0},\n{"bios9", 0xFFFF0000u, 0x20u, nullptr, nullptr, 0, 0, 0},\n};\n')
            subprocess.run([sys.executable, str(ROOT/'tools/soulsilver/tables_only.py'), str(src), str(tmp/'out')], check=True, capture_output=True, text=True)
            self.assertIn('{"bios9", 0xFFFF0000u, 0x20u, nullptr,', (tmp/'out/rc_tables.cpp').read_text())

    def test_relocated_overlay_images_use_the_same_origin_as_watch_tables(self):
        # Overlay 19 is 0xA0 bytes lower in both French games than in the US layout.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            src = tmp / 'source'
            src.mkdir()
            (src / 'apps_table.cpp').write_text('// Synthetic application metadata\n')
            (src / 'rc_tables.cpp').write_text(
                'const unsigned rc_num_chunks = 0;\nRcSection rc_sections[] = {\n'
                '{"OVY_19", 0x022598C0u, 0x880u, nullptr, nullptr, 0, 0, 0},\n};\n')
            subprocess.run([sys.executable, str(ROOT / 'tools/soulsilver/tables_only.py'),
                            str(src), str(tmp / 'out')], check=True, capture_output=True, text=True)
            table = (tmp / 'out/rc_tables.cpp').read_text()
            header = (tmp / 'out/portable_images.h').read_text()
            portable = (tmp / 'out/rc_portable.cpp').read_text()
            self.assertIn('{"OVY_19", 0x02259820u, 0x920u, nullptr,', table)
            self.assertIn('case 19: return 0x02259820u;', header)
            self.assertIn('case 19: return 0x920u;', header)
            self.assertIn('"IPGF"', portable)
            self.assertIn('"IPKF"', portable)
            self.assertIn('"IPGE"', portable)
            self.assertIn('"IPKE"', portable)
