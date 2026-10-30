"""Text-independent touch prompt detection, with synthetic button artwork."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlatinumIntroTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_labels_do_not_hide_touch_only_buttons(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = tmp/'intro.cpp'
            source.write_text(r'''#include "platinum.h"
#include <algorithm>
#include <cassert>
int main() {
    uint32_t pixels[256*192];
    std::fill(pixels, pixels+256*192, 0x5454fb);
    assert(PtFuncs::IntroTouch(pixels)==0);
    for(int y=70;y<92;y++)for(int x=100;x<140;x++)pixels[y*256+x]=0xfb5959;
    for(int y=110;y<132;y++)for(int x=100;x<140;x++)pixels[y*256+x]=0x499adb;
    // Lettering covers every pixel the old detector sampled.
    pixels[76*256+108]=pixels[84*256+132]=pixels[116*256+108]=pixels[124*256+132]=0xffffff;
    assert(PtFuncs::IntroTouch(pixels)==PtFuncs::INTRO_YESNO);
    assert(PtFuncs::IntroTouch(nullptr)==0);
}
''')
            binary = tmp/'intro'
            subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'frontend'),'-I'+str(ROOT/'melonDS/src'),str(source),'-o',str(binary)],check=True,capture_output=True,text=True)
            subprocess.run([str(binary)],check=True)
