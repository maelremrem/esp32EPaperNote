#!/usr/bin/env python3
"""Exercise production warning rasterization with a host pixel transport."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = r'''#pragma once
#include <cassert>
namespace display {
class EpaperDisplay {
public:
    bool pixels[100][100]{};
    void drawPixel(int x, int y, bool black=true) {
        assert(x>=0 && y>=0 && x<100 && y<100); pixels[y][x]=black;
    }
    void fillRect(int x, int y, int w, int h, bool black=true) {
        for(int j=0;j<h;++j) for(int i=0;i<w;++i) drawPixel(x+i,y+j,black);
    }
};
}
'''
TEST = r'''#include <iostream>
#include "display/ui_icons.h"
#define CHECK(c) do { if (!(c)) {std::cerr<<__LINE__<<": "<<#c<<"\n"; return 1;} } while(0)
int main() {
    for(int scale : {1,2,4}) {
        display::EpaperDisplay normal, inverse;
        for(auto &row:inverse.pixels) for(bool &p:row) p=true;
        display::icons::draw(normal,display::icons::Icon::Warning,10,10,scale);
        display::icons::draw(inverse,display::icons::Icon::Warning,10,10,scale,false);
        const int size=16*scale;
        int previous=-1; bool fine_step=false; int ink=0;
        for(int y=0;y<100;++y) for(int x=0;x<100;++x) {
            CHECK(normal.pixels[y][x] != inverse.pixels[y][x]);
            if(normal.pixels[y][x]) CHECK(x>=10 && y>=10 && x<10+size && y<10+size);
        }
        for(int y=0;y<size;++y) {
            int first=-1;
            for(int x=0;x<size;++x) {
                CHECK(normal.pixels[y+10][x+10] == normal.pixels[y+10][10+size-1-x]);
                if(normal.pixels[y+10][x+10]) {if(first<0) first=x; ++ink;}
            }
            if(first>=0) {if(previous>=0 && previous-first==1) fine_step=true; previous=first;}
        }
        CHECK(ink>0);
        if(scale==4) CHECK(fine_step); // Not a nearest-neighbour 4px staircase.
    }
    std::cout<<"PASS warning bounds, symmetry, native-resolution slopes and inverse mask\n";
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='warning-icon-', dir=os.environ.get('TMPDIR')) as tmp:
        path = Path(tmp)
        (path/'display').mkdir()
        (path/'display/epaper_display.h').write_text(HEADER)
        (path/'test.cpp').write_text(TEST)
        subprocess.run(shlex.split(os.environ.get('CXX','g++')) + ['-std=c++17','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('HOST_TEST_FLAGS','')), '-I'+str(path), '-I'+str(ROOT/'firmware/src'), str(path/'test.cpp'), '-o', str(path/'test')], check=True)
        subprocess.run([str(path/'test')], check=True)

if __name__ == '__main__':
    main()
