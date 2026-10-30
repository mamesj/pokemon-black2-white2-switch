"""Malformed profile headers must not crash the native function watcher."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FuncWatchParsingTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_comments_and_invalid_signatures(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            profile = tmp/'funcs.txt'
            profile.write_text('# Address profile has a descriptive header\n   # Indented comment\n02001000 t main BAD invalid\n02001000 t main - valid\n')
            source = tmp/'check.cpp'
            source.write_text('#include "funcwatch.h"\nint main(int argc,char**argv){FuncWatch f; return argc==2 && f.Load(argv[1]) && f.Count()==1 ? 0 : 1;}\n')
            binary = tmp/'check'
            subprocess.run(['c++', '-std=c++17', '-I'+str(ROOT/'frontend'), '-I'+str(ROOT/'melonDS/src'), str(source), '-o', str(binary)], check=True, capture_output=True, text=True)
            subprocess.run([str(binary), str(profile)], check=True)
