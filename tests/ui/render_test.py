"""Host tests execute production UI and production framebuffer/font drawing.
Hardware-only functions are replaced; no ESP-IDF installation is required.
Run: python3 tests/ui/render_test.py
Preview: python3 tests/ui/render_test.py --preview docs/screens/new
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import sys
import random
import struct
import zlib
import shlex
import re

ROOT = Path(__file__).resolve().parents[2]
DISPLAY = ROOT / 'firmware/src/display'

class Renderer:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        self.path = Path(self.tmp.name)
        (self.path / 'driver').mkdir()
        (self.path / 'driver/spi_master.h').write_text('using spi_device_handle_t = void*;\n')
        source = (DISPLAY / 'epaper_display.cpp').read_text()
        drawing = source[source.index('void EpaperDisplay::clear('):source.index('void EpaperDisplay::refresh()')]
        drawing = drawing.replace('    const char *p = text.c_str();', '    traces.push_back({x,y,scale,text});\n    const char *p = text.c_str();')
        drawing = drawing.replace('        return;\n    }\n    const size_t index', '        ++clipped; return;\n    }\n    const size_t index')

        harness = '''#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#define private public
#include "display/ui.h"
#undef private
#include "display/font8x12.h"
#include "display/text_layout.h"
#include "display/ui_icons.h"
struct Trace {int x,y,scale; std::string text;};
std::vector<Trace> traces;
int clipped=0, refreshes=0;
namespace display {
''' + drawing + '''
void EpaperDisplay::refresh() {++refreshes;}
}
int main(int argc, char **argv) {
    display::EpaperDisplay screen; screen.clear(); display::Ui ui(screen);
    std::string mode=argv[1], id=argv[2], text=argv[3];
    size_t index=0; try { index=static_cast<size_t>(std::stoul(id)); } catch (...) {}
    bool wifi=std::string(argv[4])=="1";
    if(mode=="idle") ui.showIdle(id,text,0,wifi);
    if(mode=="pending") ui.showIdle(id,text,123456789,wifi);
    if(mode=="live") ui.showLiveRecording(id, text, wifi);
    if(mode=="recording") ui.showRecording(id);
    if(mode=="offline") ui.showOffline(123456789,id,text);
    if(mode=="offline-empty") ui.showOffline(0,"","");
    if(mode=="sync") ui.showSyncing(999,3,id);
    if(mode=="sync-preview") {ui.setStatus(wifi,true);ui.showSyncing(1,3,id);}
    if(mode=="sync-stopping") {ui.setStatus(wifi,true);ui.showSyncStopping();}
    if(mode=="sync-cancelled") {ui.setStatus(wifi,true);ui.showInfo("Sync cancelled",text);}
    if(mode=="sync-huge") ui.showSyncing(static_cast<size_t>(-1),1,id);
    if(mode=="sync-zero") ui.showSyncing(0,0,id);
    if(mode=="menu") ui.showMenu(2);
    if(mode=="refresh") ui.showRefreshInterval(std::stoul(id));
    if(mode=="settings") {ui.setStatus(wifi,true); ui.showMenu(1);}
    if(mode=="settings-no-sd") {ui.setStatus(wifi,false); ui.showMenu(index);}
    if(mode=="submenu") ui.showSubmenu("Storage", {"Mount", "Format", "Eject", "Details", "Repair", "Usage", "Back"}, index);
    if(mode=="wifi-menu") ui.showSubmenu("Wi-Fi", {"ESP32 IP", "Saved networks", "Start portal", "Reconnect", "Open web settings", "Back"}, index, {true,true,true,false,true,false});
    if(mode=="usage") ui.showStorageMenu(index, text!="unknown", 1000, text=="full" ? 0 : text=="half" ? 500 : 1000);
    if(mode=="usage-huge") ui.showStorageMenu(index,true,UINT64_MAX,UINT64_MAX/2);
    if(mode=="storage-menu") ui.showStorageMenu(index,true,32ULL*1024*1024*1024,16ULL*1024*1024*1024);
    if(mode=="submenu-flags") ui.showSubmenu("Options", {"First", "Second", "Third"}, index,
                                             text=="short" ? std::vector<bool>{true} : std::vector<bool>{false,true,false,true});
    if(mode=="cancel-pending") ui.showCancelConfirmation(index!=0,false,text);
    if(mode=="cancel-all") ui.showCancelConfirmation(index!=0,true,"");
    if(mode=="sync-menu") ui.showSubmenu("Synchronization",{"Sync now","Pending notes","Cancel all pending","Back"},index,{false,true,true,false});
    if(mode=="format-cancel") ui.showFormatConfirmation(false);
    if(mode=="format-erase") ui.showFormatConfirmation(true);
    if(mode=="portal") ui.showPortal("Whistle-abcdef", "Ab3dEf5g", "192.168.4.1");
    if(mode=="portal-custom") ui.showPortal(id, text, "192.168.4.1");
    if(mode=="sd-error") ui.showStorageError();
    if(mode=="notes" || mode=="notes-last" || mode=="notes-huge") {
        std::vector<display::SavedNote> notes;
        for(int i=0;i<8;++i) notes.push_back({"Note 0"+std::to_string(i+1),i%2==1});
        ui.showSavedNotes(notes,mode=="notes" ? 0 : (mode=="notes-last" ? 8 : static_cast<size_t>(-1)));
    }
    if(mode=="notes-empty") ui.showSavedNotes({},0);
    if(mode=="list-notes" || mode=="list-submenu") {
        std::vector<display::SavedNote> notes;
        std::vector<std::string> entries;
        for(size_t i=0;i<std::stoul(text);++i) {
            notes.push_back({"Note "+std::to_string(i),true});
            entries.push_back("Option "+std::to_string(i));
        }
        if(mode=="list-notes") ui.showSavedNotes(notes,index);
        else ui.showSubmenu("Options",entries,index);
    }
    if(mode=="reader") ui.showNote(id,text,1);
    if(mode=="reader-huge") ui.showNote(id,text,static_cast<size_t>(-1));
    if(mode=="info") ui.showInfo(id,text);
    if(mode=="error") ui.showError(id,text);
    if(mode=="error-fatal") ui.showError(id,text,false);
    if(mode=="boot") {ui.setStatus(false,false);ui.showBoot();}
    if(mode=="boot-sd-ready" || mode=="boot-sd-failed") {
        ui.setStatus(false,mode=="boot-sd-ready");
        ui.showBootProgress(1,"SD card",mode=="boot-sd-ready" ? "Ready" : "Unavailable. Settings can retry.");
    }
    if(mode=="boot-audio") {ui.setStatus(false,true);ui.showBootProgress(1,"Audio","Initializing");}
    if(mode=="boot-server-checking") {ui.setStatus(true,true);ui.showBootProgress(3,"Server","Checking /health");}
    if(mode=="boot-server-ready") {ui.setStatus(true,true);ui.showBootProgress(4,"Server","Ready");}
    if(mode=="boot-server-failed") {ui.setStatus(true,true);ui.showBootProgress(4,"Server","Unavailable. Sync can retry.");}
    if(mode=="boot-server-skipped") {ui.setStatus(false,true);ui.showBootProgress(4,"Server","Skipped: no Wi-Fi");}

    if(mode=="boot-progress") ui.showBootProgress(index, "Server", text);
    if(mode=="boot-wifi") {
        auto split=id.find(',');
        ui.setStatus(std::stoi(id.substr(0,split))==2 || std::stoi(id.substr(split+1))==2, true);
        ui.showBootWifi(static_cast<display::BootWifiStatus>(std::stoi(id.substr(0,split))),
                        static_cast<display::BootWifiStatus>(std::stoi(id.substr(split+1))),text);
    }
    if(mode=="raw") screen.drawText(8,8,text);
    if(mode=="raw-inverse") {screen.clear(false); screen.drawText(8,8,text,1,false);}
    if(mode=="raw-scale") screen.drawText(8,8,text,3);
    if(mode=="raw-edge") screen.drawText(192,188,text);
    if(mode=="raw-bottom") screen.drawText(8,194,text);
    if(mode=="raw-right") screen.drawText(196,8,text);
    for(auto &t: traces) {
        std::cout<<t.x<<'\\t'<<t.y<<'\\t'<<t.scale<<'\\t';
        for(unsigned char c: t.text) std::cout<<"0123456789abcdef"[c>>4]<<"0123456789abcdef"[c&15];
        std::cout<<'\\n';
    }
    std::cout<<"META\\t"<<clipped<<'\\t'<<refreshes<<'\\n';
    if(argc>5) {std::ofstream out(argv[5],std::ios::binary); out<<"P4\\n200 200\\n";
        for(auto b:screen.framebuffer_) out.put(static_cast<char>(~b));}
}
'''
        (self.path / 'render.cpp').write_text(harness)
        flags=shlex.split(os.environ.get('UI_TEST_CXXFLAGS',''))
        cxx=os.environ.get('CXX','g++')
        cc=os.environ.get('CC','clang' if 'clang' in cxx else 'gcc')
        # Compile upstream as C, just like ESP-IDF, including sanitizer instrumentation.
        subprocess.run([cc,'-std=c99','-Wall','-Wextra','-Werror',*flags,'-c',
                        str(DISPLAY/'qr/qrcodegen.c'),'-o',str(self.path/'qr.o')],check=True)
        subprocess.run([cxx,'-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I'+str(self.path),'-I'+str(ROOT/'firmware/src'),str(self.path/'render.cpp'),str(DISPLAY/'ui.cpp'),str(DISPLAY/'wifi_qr.cpp'),str(self.path/'qr.o'),'-o',str(self.path/'render')],check=True)

    def render(self, mode, text='', note='000123', wifi=False, output=None):
        args=[str(self.path/'render'),mode,note,text,'1' if wifi else '0']
        if output: args.append(str(output))
        rows=subprocess.check_output(args,text=True).splitlines()
        traces=[]
        meta=(-1,-1)
        for row in rows:
            parts=row.split('\t',3)
            if parts[0]=='META': meta=tuple(map(int,parts[1:])); continue
            traces.append((int(parts[0]),int(parts[1]),int(parts[2]),bytes.fromhex(parts[3]).decode('utf-8')))
        return traces,meta

class LayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.renderer=Renderer()
    def test_pending_confirmation_preserves_audio_and_defaults_to_back(self):
        for mode in ('cancel-pending','cancel-all'):
            traces,meta=self.renderer.render(mode,text='20261007T120000Z-0001',note='0')
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
            words=' '.join(t[3] for t in traces)
            self.assertIn('Back',words); self.assertIn('Keep audio',words); self.assertIn('Cancel pending',words)
            if mode=='cancel-pending': self.assertIn('20261007T120000Z-0001',words)

    def test_storage_gauge_real_framebuffer(self):
        for mode, state, filled in [('usage','empty',0),('usage','half',88),('usage','full',176),('usage','unknown',0),('usage-huge','',88)]:
            out=self.renderer.path/'usage.pbm'
            traces,meta=self.renderer.render(mode,text=state,note='0',output=out)
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
            data=out.read_bytes().split(b'\n',2)[2]
            def black(x,y): return bool(data[y*25+x//8] & (0x80>>(x%8)))
            self.assertEqual(sum(black(x,120) for x in range(10,186)),filled)
            self.assertTrue(any('Usage unknown' in t[3] for t in traces) if state=='unknown' else any('used' in t[3] for t in traces))

    def test_settings_and_submenu_scrollbar_tracks_displayed_range(self):
        for mode in ('settings-no-sd', 'submenu'):
            for selected in (0, 5, 6, 999999):
                with self.subTest(mode=mode, selected=selected):
                    output=self.renderer.path/'scroll.pbm'
                    traces,meta=self.renderer.render(mode,note=str(selected),output=output)
                    self.assertEqual(meta,(0,1))
                    self.assert_no_collisions(traces)
                    start=0 if selected%7<6 else 1
                    self.assertTrue(all(x+len(word)*8*scale<=188 for x,row,scale,word in traces if 32<=row<158))
                    self.assert_scrollbar(output,32,126,7,start,start+6)

    def assert_scrollbar(self, output, y, height, total, first, last):
        data=output.read_bytes().split(b'\n',2)[2]
        def black(x,row): return bool(data[row*25+x//8] & (0x80>>(x%8)))
        inner=height-2
        top=first*inner//total
        bottom=(last*inner+total-1)//total
        for row in range(y,y+height):
            for x in range(189,192):
                expected=(x in (189,191) or row in (y,y+height-1) or
                          y+1+top<=row<y+1+bottom)
                self.assertEqual(black(x,row),expected,(x,row,total,first,last))
        self.assertFalse(any(black(x,row) for row in range(28,y) for x in range(189,192)))
        self.assertFalse(any(black(x,row) for row in range(y+height,164) for x in range(189,192)))

    def test_notes_scrollbar_counts_back_and_partial_last_page(self):
        for count in (0,1,4,5,8,9,10,1001):
            for selected in (0,count,999999):
                with self.subTest(count=count,selected=selected):
                    output=self.renderer.path/'notes-scroll.pbm'
                    traces,meta=self.renderer.render('list-notes',str(count),note=str(selected),output=output)
                    self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
                    self.assertTrue(all(x+len(word)*8*scale<=188 for x,row,scale,word in traces if 34<=row<146))
                    total=count+1
                    first=min(selected,count)//5*5
                    last=min(first+5,total)
                    if total>5: self.assert_scrollbar(output,34,112,total,first,last)
                    else: self.assert_no_scrollbar(output)

    def assert_no_scrollbar(self, output):
        data=output.read_bytes().split(b'\n',2)[2]
        self.assertFalse(any(data[y*25+x//8] & (0x80>>(x%8))
                             for y in range(28,164) for x in range(189,192)))

    def test_short_submenus_have_no_scrollbar(self):
        for count in (0,1,6):
            for selected in (0,999999):
                output=self.renderer.path/'short-scroll.pbm'
                traces,meta=self.renderer.render('list-submenu',str(count),note=str(selected),output=output)
                self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
                self.assert_no_scrollbar(output)

    def test_long_submenus_have_bounded_positive_thumbs_at_extremes(self):
        for count in (7,19,1001):
            for selected in (0,count-1,999999):
                output=self.renderer.path/'long-submenu.pbm'
                traces,meta=self.renderer.render('list-submenu',str(count),note=str(selected),output=output)
                self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
                chosen=selected%count
                first=max(0,chosen-5)
                self.assert_scrollbar(output,32,126,count,first,min(first+6,count))

    def test_missing_or_extra_child_flags_are_safe(self):
        for flags,children in (('short',{0}),('long',{1})):
            for selected in range(3):
                output=self.renderer.path/'flags.pbm'
                traces,meta=self.renderer.render('submenu-flags',flags,note=str(selected),output=output)
                self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
                self.assert_chevrons(output,traces,[(32+i*21,i in children,i==selected) for i in range(3)])

    def test_settings_child_views_keep_chevrons_alongside_state(self):
        for selected in range(7):
            output=self.renderer.path/'arrows.pbm'
            traces,meta=self.renderer.render('settings-no-sd',note=str(selected),output=output)
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
            start=0 if selected<6 else 1
            children={0,1,2,3,4,5}
            self.assert_chevrons(output,traces,[(32+(i-start)*21,i in children,i==selected)
                                               for i in range(start,start+6)])
            self.assertIn('No SD',[t[3] for t in traces])
            self.assertIn('10',[t[3] for t in traces])

    def assert_chevrons(self, output, traces, rows):
        data=output.read_bytes().split(b'\n',2)[2]
        reference=self.renderer.path/'chevron.pbm'
        self.renderer.render('raw','>',output=reference)
        mask=reference.read_bytes().split(b'\n',2)[2]
        def black(p,x,y): return bool(p[y*25+x//8] & (0x80>>(x%8)))
        for y,child,selected in rows:
            self.assertEqual(any(x==172 and row==y+4 and word=='>' for x,row,_,word in traces),child)
            if child:
                for dy in range(12):
                    for dx in range(8):
                        self.assertEqual(black(data,172+dx,y+4+dy),black(mask,8+dx,8+dy)^selected)

    def test_submenu_child_indicators_are_explicit_not_action_labels(self):
        for mode,children,total in (('wifi-menu',{0,1,2,4},6),('storage-menu',{0,1},3)):
            for selected in range(total):
                output=self.renderer.path/'submenu-arrows.pbm'
                traces,meta=self.renderer.render(mode,note=str(selected),output=output)
                self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
                self.assert_no_scrollbar(output)
                self.assert_chevrons(output,traces,[(32+i*21,i in children,i==selected) for i in range(total)])
        # Legacy callers and arbitrary labels must not imply navigation.
        output=self.renderer.path/'legacy-submenu.pbm'
        traces,_=self.renderer.render('submenu',note='0',output=output)
        self.assert_chevrons(output,traces,[(32+i*21,False,i==0) for i in range(6)])

    def test_refresh_interval_and_seventh_menu_item_fit(self):
        for limit in (0,1,5,10,20,50,100):
            traces,meta=self.renderer.render('refresh',note=str(limit))
            words=[t[3] for t in traces]
            self.assertIn('Full refresh',words)
            self.assertIn('Full only' if limit==0 else str(limit)+' partial updates',words)
            self.assertIn('Save',words)
            self.assertEqual(meta,(0,1))
            self.assert_no_collisions(traces)
        traces,meta=self.renderer.render('settings-no-sd',note='6')
        self.assertIn('Full refresh',[t[3] for t in traces])
        self.assertIn('10',[t[3] for t in traces])
        self.assert_no_collisions(traces)

    def assert_no_collisions(self,traces):
        boxes=[]
        for x,y,s,text in traces:
            self.assertLessEqual(x+len(text)*8*s,192)
            self.assertLessEqual(y+12*s,196)
            boxes.append((x,y,x+len(text)*8*s,y+12*s))
        for i,a in enumerate(boxes):
            for b in boxes[i+1:]:
                self.assertFalse(a[0]<b[2] and b[0]<a[2] and a[1]<b[3] and b[1]<a[3])

    def test_boot_circle_tracks_completed_stages_and_clips_status(self):
        import math
        frames=[]
        for step in (0,2,4,999):
            output=self.renderer.path/('boot-'+str(step)+'.pbm')
            traces,meta=self.renderer.render('boot-progress','Unavailable '+('x'*100),note=str(step),output=output)
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
            self.assertIn('Starting',[t[3] for t in traces])
            self.assertIn(str(min(step,4))+'/4',[t[3] for t in traces])
            self.assertTrue(any(t[1]==118 and t[3]=='Server' for t in traces))
            pixels=output.read_bytes().split(b'\n',2)[2]
            ink={(x,y) for y in range(40,114) for x in range(60,140)
                 if 27<=math.hypot(x-100,y-78)<31 and pixels[y*25+x//8] & (0x80>>(x%8))}
            frames.append(ink)
        self.assertFalse(frames[0]); self.assertTrue(frames[1]<frames[2]); self.assertEqual(frames[2],frames[3])
        self.assertTrue((129,78) in frames[1] and (71,78) not in frames[1])

    def test_boot_wifi_reports_each_state_without_inventing_success_or_ip(self):
        labels=['Pending','Connecting','Connected','Failed','Skipped','Disabled']
        for home,home_label in enumerate(labels):
            for hotspot,hotspot_label in enumerate(labels):
                for ip in ['', '192.168.123.254']:
                    with self.subTest(home=home_label,hotspot=hotspot_label,ip=ip):
                        output=self.renderer.path/'boot-wifi.pbm'
                        traces,meta=self.renderer.render('boot-wifi',ip,note=f'{home},{hotspot}',output=output)
                        words=[t[3] for t in traces]
                        self.assertIn('Home Wi-Fi',words)
                        self.assertIn('Hotspot',words)
                        states=[word for word in words if word in labels]
                        self.assertEqual(states,[home_label,hotspot_label])
                        self.assertEqual(words.count('Connected'),[home_label,hotspot_label].count('Connected'))
                        self.assertEqual([word for word in words if word.startswith('IP ')],
                                         ['IP '+ip] if ip else [])
                        self.assertEqual(meta,(0,1))
                        pixels=output.read_bytes().split(b'\n',2)[2]
                        self.assertEqual(len(pixels),5000)
                        self.assertTrue(any(pixels),'actual framebuffer must contain ink')
                        boxes=[]
                        for x,y,scale,text in traces:
                            self.assertGreaterEqual(x,8)
                            self.assertLessEqual(x+len(text)*8*scale,192)
                            self.assertGreaterEqual(y,0)
                            self.assertLessEqual(y+12*scale,196)
                            boxes.append((x,y,x+len(text)*8*scale,y+12*scale))
                        for i,a in enumerate(boxes):
                            for b in boxes[i+1:]:
                                self.assertFalse(a[0]<b[2] and b[0]<a[2] and a[1]<b[3] and b[1]<a[3])

    def test_bold_pixels_preserve_original_ink_and_cell_metrics(self):
        # Immutable regular bitmap remains the baseline, not a duplicated renderer.
        rows=re.findall(r'\{((?:0x[0-9A-Fa-f]{2},?\s*){12})\}', (DISPLAY/'font8x12.h').read_text())
        self.assertEqual(len(rows),95)
        for index,row in enumerate(rows):
            glyph=[int(value,16) for value in re.findall(r'0x[0-9A-Fa-f]{2}',row)]
            character=chr(index+32)
            with self.subTest(character=character):
                output=self.renderer.path/'glyph.pbm'
                self.renderer.render('raw',character,output=output)
                pixels=output.read_bytes().split(b'\n',2)[2]
                ink={(x,y) for y in range(200) for x in range(200)
                     if pixels[y*25+x//8] & (0x80>>(x%8))}
                original={(8+x,8+y) for y,bits in enumerate(glyph) for x in range(8)
                          if bits & (0x80>>x)}
                self.assertTrue(original<=ink,'bold must never erase original edge pixels')
                self.assertTrue(all(8<=x<16 and 8<=y<20 for x,y in ink),'8x12 cell unchanged')
                if original: self.assertGreater(len(ink),len(original),'actual pixels must be thicker')
                else: self.assertFalse(ink,'space must stay empty')

    def test_bold_inverse_scale_advance_and_edge_cells(self):
        normal=self.renderer.path/'normal.pbm'
        inverse=self.renderer.path/'inverse.pbm'
        self.renderer.render('raw','MW@',output=normal)
        self.renderer.render('raw-inverse','MW@',output=inverse)
        a=normal.read_bytes().split(b'\n',2)[2]
        b=inverse.read_bytes().split(b'\n',2)[2]
        self.assertEqual(a,bytes(value^255 for value in b),'inverse draws the same bold mask')
        scaled=self.renderer.path/'scale.pbm'
        self.renderer.render('raw-scale','MW@',output=scaled)
        s=scaled.read_bytes().split(b'\n',2)[2]
        def pixel(data,x,y): return bool(data[y*25+x//8] & (0x80>>(x%8)))
        for y in range(36):
            for x in range(72):
                self.assertEqual(pixel(s,8+x,8+y),pixel(a,8+x//3,8+y//3))
        edge=self.renderer.path/'edge.pbm'
        _,meta=self.renderer.render('raw-edge','M',output=edge)
        self.assertEqual(meta,(0,0),'full 8x12 bottom-right glyph needs no clipping')
        e=edge.read_bytes().split(b'\n',2)[2]
        for y in range(12):
            for x in range(8): self.assertEqual(pixel(e,192+x,188+y),pixel(a,8+x,8+y))

    def test_raw_advance_space_and_newline_metrics_remain_8_by_12(self):
        single=self.renderer.path/'single.pbm'
        multiple=self.renderer.path/'multiple.pbm'
        self.renderer.render('raw','I',output=single)
        self.renderer.render('raw','I I\nI',output=multiple)
        def ink(path):
            data=path.read_bytes().split(b'\n',2)[2]
            return {(x,y) for y in range(200) for x in range(200)
                    if data[y*25+x//8] & (0x80>>(x%8))}
        original=ink(single)
        self.assertEqual(ink(multiple),original | {(x+16,y) for x,y in original} |
                         {(x,y+14) for x,y in original})

    def test_home_matches_reference_with_large_microphone(self):
        output=self.renderer.path/'home.pbm'
        traces,meta=self.renderer.render('idle',note='',output=output)
        words=' '.join(t[3] for t in traces)
        self.assertIn('RECORD',words)
        self.assertIn('Short',words)
        self.assertIn('Settings',words)
        pixels=output.read_bytes().split(b'\n',2)[2]
        def black(x,y): return bool(pixels[y*25+x//8] & (0x80>>(x%8)))
        self.assertTrue(black(100,53), 'large black microphone medallion')
        self.assertFalse(black(100,80), 'white microphone inside medallion')
        self.assertEqual(meta,(0,1))
    def test_idle_has_only_real_telemetry_and_actions(self):
        traces,meta=self.renderer.render('idle','Une note a relire.')
        text=' '.join(t[3] for t in traces)
        self.assertNotIn('BAT',text)
        self.assertNotIn('SD',text)
        self.assertNotIn('suivant',text)
        self.assertNotIn('SYNCED',text)
        self.assertIn('Offline',text)
        self.assertEqual(meta,(0,1))

    def test_settings_reference_has_icons_and_inverted_selection(self):
        output=self.renderer.path/'settings.pbm'
        traces,meta=self.renderer.render('settings',wifi=True,output=output)
        words=' '.join(t[3] for t in traces)
        for label in ['Settings','Notes','Sync','Wi-Fi','Storage','Full refresh','Next','Select']:
            self.assertIn(label,words)
        self.assertNotIn('Luminosite',words)
        pixels=output.read_bytes().split(b'\n',2)[2]
        self.assertTrue(pixels[55*25+10//8] & (0x80>>(10%8)))
        self.assertEqual(meta,(0,1))

    def test_submenu_scrolls_six_rows_and_keeps_truthful_actions(self):
        entries=['Mount','Format','Eject','Details','Repair','Usage','Back']
        for selected in range(len(entries)):
            traces,meta=self.renderer.render('submenu',note=str(selected))
            words=[t[3] for t in traces]
            self.assertIn(entries[selected],words)
            self.assertIn('Next',words); self.assertIn('Select',words)
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)

    def test_format_confirmation_defaults_to_cancel_and_toggles_erase(self):
        for mode,action in [('format-cancel','Cancel'),('format-erase','Erase')]:
            traces,meta=self.renderer.render(mode)
            words=[t[3] for t in traces]
            self.assertIn('Erase all card data?',words)
            self.assertIn('ALL notes and recordings', ' '.join(words))
            self.assertIn('Erase SD',words); self.assertIn(action,words)
            self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)

    def test_portal_qr_decodes_actual_framebuffer(self):
        import zxingcpp
        from PIL import Image
        output=self.renderer.path/'portal-qr.pbm'
        traces,meta=self.renderer.render('portal',output=output)
        decoded=zxingcpp.read_barcodes(Image.open(output))
        self.assertEqual([code.text for code in decoded],
                         ['WIFI:T:WPA;S:Whistle-abcdef;P:Ab3dEf5g;;'])
        self.assertEqual(meta,(0,1))
        self.assert_no_collisions(traces)

    def test_portal_maximum_ssid_special_characters_roundtrip(self):
        import zxingcpp
        from PIL import Image
        def escape(value):
            return ''.join(('\\' if c in '\\;,:"' else '')+c for c in value)
        def unescape(value):
            result=[]; escaped=False
            for c in value:
                if escaped: result.append(c); escaped=False
                elif c=='\\': escaped=True
                else: result.append(c)
            self.assertFalse(escaped)
            return ''.join(result)
        ssid=('\\;,:"'*7)[:32]
        password='a\\;,:"8Z'
        output=self.renderer.path/'portal-special.pbm'
        traces,meta=self.renderer.render('portal-custom',password,note=ssid,output=output)
        decoded=zxingcpp.read_barcodes(Image.open(output))
        payload='WIFI:T:WPA;S:'+escape(ssid)+';P:'+escape(password)+';;'
        self.assertEqual([code.text for code in decoded],[payload])
        self.assertEqual(unescape(decoded[0].text[13:].split(';P:')[0]),ssid)
        self.assertEqual(unescape(decoded[0].text.split(';P:')[1][:-2]),password)
        self.assertEqual(''.join(t[3] for t in traces if t[1] in (120,134)),ssid)
        self.assertIn('PW: '+password,[t[3] for t in traces])
        self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
        # Worst-case escaped fixture forces version 5: 37 modules, exact 2px cells,
        # white 4-module border at x55..144, y28..117. Verify actual finder pixels.
        from PIL import Image as PILImage
        image=PILImage.open(output).convert('L')
        for y in range(28,118):
            for x in range(55,145):
                if x<63 or x>=137 or y<36 or y>=110:
                    self.assertEqual(image.getpixel((x,y)),255,(x,y))
        for y in range(7):
            for x in range(7):
                black=x in (0,6) or y in (0,6) or (2<=x<=4 and 2<=y<=4)
                for dy in range(2):
                    for dx in range(2):
                        self.assertEqual(image.getpixel((63+2*x+dx,36+2*y+dy)),0 if black else 255)

    def test_portal_invalid_or_unencodable_input_has_manual_fallback(self):
        import zxingcpp
        from PIL import Image
        fixtures=[('s'*33,'Ab3dEf5g'),('s'*4096,'p'*4096),('', 'Ab3dEf5g'),
                  ('Valid','short'),('Valid','x'*64),('bad'+chr(10)+'ssid','Ab3dEf5g'),
                  (';'*32,';'*63)] # Valid WPA length, but escaped QR exceeds v5.
        for ssid,password in fixtures:
            with self.subTest(ssid_length=len(ssid),password_length=len(password)):
                output=self.renderer.path/'portal-fallback.pbm'
                traces,meta=self.renderer.render('portal-custom',password,note=ssid,output=output)
                self.assertEqual(zxingcpp.read_barcodes(Image.open(output)),[])
                words=[t[3] for t in traces]
                self.assertIn('QR unavailable',words)
                self.assertIn('Connect manually',words)
                self.assertIn('192.168.4.1',words)
                self.assertIn('Close (10 min)',words)
                self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)

    def test_portal_contains_complete_credentials_and_fits_panel(self):
        traces,meta=self.renderer.render('portal')
        words=[t[3] for t in traces]
        for label in ['Wi-Fi setup','Whistle-abcdef','PW: Ab3dEf5g','192.168.4.1','Close (10 min)']:
            self.assertIn(label,words)
        self.assertEqual(words.count('Close (10 min)'),1); self.assertEqual(meta,(0,1))
        self.assert_no_collisions(traces)

    def test_sd_error_is_explicit_and_offers_settings_without_restart(self):
        traces,meta=self.renderer.render('sd-error')
        words=' '.join(t[3] for t in traces)
        self.assertIn('SD unavailable',words)
        self.assertIn('FAT32',words)
        self.assertIn('Short',words)
        self.assertIn('Menu',words)
        self.assertIn('Long',words)
        self.assertIn('Settings',words)
        self.assertNotIn('Restart',words)
        self.assertNotIn('Record',words)
        self.assertEqual(meta,(0,1))

    def test_all_menu_entries_remain_selectable_without_sd(self):
        outputs=[]
        for selected in range(7):
            output=self.renderer.path/('menu-'+str(selected)+'.pbm')
            traces,meta=self.renderer.render('settings-no-sd',note=str(selected),output=output)
            words=[t[3] for t in traces]
            entries=['Notes','Sync','Wi-Fi','Storage','Full refresh','About','Back']
            visible=entries[:6] if selected<6 else entries[1:]
            for label in ['Settings','No SD','Next','Select']+visible:
                self.assertIn(label,words)
            self.assertEqual(meta,(0,1))
            outputs.append(output.read_bytes())
        self.assertEqual(len(set(outputs)),7,'selection changes on every menu entry without SD')
        traces,meta=self.renderer.render('info','Insert a FAT32 card.\nOpen Storage to retry.',note='Storage')
        self.assertIn('Storage',[t[3] for t in traces])
        self.assertEqual([t[3] for t in traces].count('Back'),2)
        self.assertEqual(meta,(0,1))

    def test_builtin_messages_are_english_but_transcripts_are_not_translated(self):
        expected={
            'idle':['Notebook','Audio on SD card','Hold for menu','Offline','Record','Settings'],
            'pending':['123456789 pending'],
            'recording':['Speak, I am listening','Local audio','Stop'],
            'sync':['Syncing','Sending to transcriber','Wait','Cancel'],
            'error':['Warning','Record','Settings'],
            'reader':['Reader','Empty text','Back'],
            'notes-empty':['No notes','Back'],
        }
        for mode,labels in expected.items():
            with self.subTest(mode=mode):
                traces,meta=self.renderer.render(mode)
                words=[t[3] for t in traces]
                for label in labels: self.assertIn(label,words)
                self.assertEqual(meta,(0,1))
        for mode in ['idle','live']:
            traces,_=self.renderer.render(mode,'Bonjour le monde\nHola mundo\nGuten Tag')
            self.assertEqual([t[3] for t in traces if 48<=t[1]<150],
                             ['Bonjour le monde','Hola mundo','Guten Tag'])

    def test_fatal_error_does_not_offer_unavailable_button_actions(self):
        traces,meta=self.renderer.render('error-fatal','Codec unavailable',note='Audio')
        words=[t[3] for t in traces]
        self.assertIn('Restart',words)
        self.assertNotIn('Settings',words)
        self.assertNotIn('Record',words)
        self.assertEqual(meta,(0,1))

    def test_saved_notes_scroll_and_always_offer_a_return(self):
        traces,meta=self.renderer.render('notes')
        words=[t[3] for t in traces]
        self.assertIn('Note 01',words)
        self.assertIn('Note 05',words)
        self.assertNotIn('Note 06',words)
        self.assertIn('Open',words)
        self.assertEqual(meta,(0,1))
        traces,meta=self.renderer.render('notes-last')
        self.assertIn('Back',[t[3] for t in traces])
        self.assertNotIn('Note 01',[t[3] for t in traces])
        self.assertEqual(meta,(0,1))
        huge,_=self.renderer.render('notes-huge')
        self.assertEqual(huge,traces)
        traces,meta=self.renderer.render('notes-empty')
        self.assertIn('No notes',[t[3] for t in traces])
        self.assertIn('Back',[t[3] for t in traces])
        self.assertEqual(meta,(0,1))

    def test_reader_paginates_without_losing_transcript_lines(self):
        transcript='\n'.join('ligne '+str(i) for i in range(16))
        traces,meta=self.renderer.render('reader',transcript)
        body=[t[3] for t in traces if 48<=t[1]<150]
        self.assertEqual(body,['ligne '+str(i) for i in range(7,14)])
        self.assertIn('Page 2/3',[t[3] for t in traces])
        self.assertEqual(meta,(0,1))
        traces,_=self.renderer.render('reader-huge',transcript)
        self.assertIn('Page 3/3',[t[3] for t in traces])
        traces,meta=self.renderer.render('reader','')
        self.assertIn('Empty text',[t[3] for t in traces])
        self.assertIn('Page 1/1',[t[3] for t in traces])
        self.assertEqual(meta,(0,1))

    def test_transcript_splits_long_words_at_glyph_boundary(self):
        traces,meta=self.renderer.render('idle','a'*80)
        body=[t for t in traces if 48<=t[1]<150]
        self.assertEqual(''.join(t[3] for t in body),'a'*80)
        self.assertTrue(all(len(t[3])<=23 for t in body))
        self.assertEqual(meta,(0,1))

    def test_accented_transcript_counts_glyphs_not_bytes(self):
        traces,_=self.renderer.render('idle','é'*23+' fin')
        body=[t[3] for t in traces if 48<=t[1]<150]
        self.assertEqual(body,['e'*23,'fin'])

    def test_static_transcript_uses_seven_lines_and_marks_only_hidden_text(self):
        traces,_=self.renderer.render('idle','\n'.join('ligne '+str(i) for i in range(9)))
        body=[t for t in traces if 48<=t[1]<150]
        self.assertEqual(len(body),7)
        self.assertTrue(body[-1][3].endswith('...'))
        traces,_=self.renderer.render('idle','\n'.join('x'*23 for _ in range(7)))
        self.assertFalse(any(t[3].endswith('...') for t in traces))

    def test_live_recording_keeps_recent_transcript_and_small_rec(self):
        transcript='\n'.join('ligne '+str(i) for i in range(12))
        traces,meta=self.renderer.render('live',transcript,wifi=True)
        body=[t[3] for t in traces if 48<=t[1]<150]
        self.assertEqual(body,['ligne '+str(i) for i in range(5,12)])
        self.assertTrue(any(t[3]=='REC' and t[2]==1 for t in traces))
        self.assertTrue(any(t[3]=='Wi-Fi' for t in traces))
        self.assertEqual(meta,(0,1))

    def test_live_offline_explains_local_recording(self):
        traces,_=self.renderer.render('live','Texte recent',wifi=False)
        words=' '.join(t[3] for t in traces)
        self.assertIn('Offline',words)
        self.assertIn('local',words)
        self.assertNotIn('Wi-Fi',words)

    def test_empty_offline_notebook_does_not_claim_saved_audio(self):
        traces,_=self.renderer.render('offline-empty')
        words=' '.join(t[3] for t in traces)
        self.assertNotIn('Audio garde',words)
        self.assertNotIn('Sauvegarde locale',words)
        self.assertIn('Your first note',words)
        self.assertIn('RECORD',words)

    def test_real_submenu_labels_and_portal_lifetime(self):
        for mode, labels in [('wifi-menu', ['ESP32 IP', 'Saved networks', 'Start portal', 'Reconnect', 'Back']),
                             ('storage-menu', ['Format SD', 'Back'])]:
            traces,meta=self.renderer.render(mode,note='0')
            self.assertEqual(meta,(0,1))
            words=[t[3] for t in traces]
            for label in labels: self.assertIn(label,words)
            self.assertIn('Next',words); self.assertIn('Select',words)
        traces,meta=self.renderer.render('portal')
        self.assertIn('Close (10 min)',[t[3] for t in traces])
        self.assertEqual(meta,(0,1))

    def test_all_screens_bound_text_before_implicit_wrap(self):
        for mode in ['boot','idle','pending','recording','live','offline','sync','menu','settings','settings-no-sd','submenu','format-cancel','format-erase','portal','sd-error','error','notes','notes-last','notes-empty','reader','reader-huge','info']:
            with self.subTest(mode=mode):
                traces,meta=self.renderer.render(mode,('trèslongmot'*30)+'\nfin',note='é'*60)
                self.assertEqual(meta,(0,1))
                for x,y,scale,text in traces:
                    self.assertGreaterEqual(x,8)
                    self.assertLessEqual(x+len(text)*8*scale,192)
                    self.assertGreaterEqual(y,0)
                    self.assertLessEqual(y+12*scale,196)

    def test_unknown_network_never_claims_wifi_or_offline(self):
        for mode in ['recording','sync','menu','error']:
            traces,_=self.renderer.render(mode,'Une note')
            self.assertIn('?', [t[3] for t in traces])

    def test_utf8_unsupported_codepoint_draws_one_fallback_cell(self):
        a=self.renderer.path/'emoji.pbm'
        b=self.renderer.path/'fallback.pbm'
        self.renderer.render('raw','😀 fin',output=a)
        self.renderer.render('raw','? fin',output=b)
        self.assertEqual(a.read_bytes(),b.read_bytes())

    def test_raw_text_never_draws_partial_glyph_at_panel_edges(self):
        for mode in ['raw-bottom','raw-right']:
            _,meta=self.renderer.render(mode,'M')
            self.assertEqual(meta,(0,0))

    def test_text_boxes_do_not_overlap_across_edge_case_transcripts(self):
        rng=random.Random(200)
        samples=['','\n\n\n','é'*400,'😀'*200,'mot\tavec\r\nsauts']
        samples += [''.join(rng.choice('abc é😀\n\t') for _ in range(300)) for _ in range(10)]
        for mode in ['idle','pending','live','offline','error','reader','info','notes','notes-last','settings','submenu','format-cancel','format-erase','portal']:
            for sample in samples:
                traces,meta=self.renderer.render(mode,sample,note='identifiant'*8)
                self.assertEqual(meta,(0,1))
                boxes=[(x,y,x+len(t)*8*s,y+12*s) for x,y,s,t in traces if t]
                for i,a in enumerate(boxes):
                    for b in boxes[i+1:]:
                        self.assertFalse(a[0]<b[2] and b[0]<a[2] and a[1]<b[3] and b[1]<a[3],(mode,a,b))

    def test_sync_stopping_has_wait_actions_not_false_back_or_cancel(self):
        traces,meta=self.renderer.render('sync-stopping')
        self.assertEqual(meta,(0,1)); self.assert_no_collisions(traces)
        words=[t[3] for t in traces]
        self.assertIn('Stopping sync',words); self.assertEqual(words.count('Wait'),2)
        self.assertNotIn('Back',words)

    def test_sync_progress_clamps_before_integer_conversion(self):
        huge=self.renderer.path/'huge.pbm'
        full=self.renderer.path/'full.pbm'
        self.renderer.render('sync-huge',output=huge)
        self.renderer.render('sync',output=full)
        # Compare only progress bar rows: huge/1 and 999/3 both saturate.
        huge_pixels=huge.read_bytes().split(b'\n',2)[2]
        full_pixels=full.read_bytes().split(b'\n',2)[2]
        self.assertEqual(huge_pixels[100*25:112*25],full_pixels[100*25:112*25])
        _,meta=self.renderer.render('sync-zero')
        self.assertEqual(meta,(0,1))

def previews(destination, boot_only=False):
    destination=Path(destination)
    destination.mkdir(parents=True,exist_ok=True)
    renderer=Renderer()
    transcript='Remember to book the train for Friday.\nCall Julie tomorrow to prepare the meeting and share the project notes.'
    recent='\n'.join('Earlier line '+str(i) for i in range(7))+'\nThe latest point:\nprepare the demo and\nconfirm with Julie.'
    cases=[('home','',True),('notes','',True),('settings','',True),('sd-error','',False),
           ('menu','',False),('settings-no-sd','',False),('submenu','',False),('wifi-menu','',False),('storage-menu','',False),('format-cancel','',False),('format-erase','',False),('portal','',False),('recording','',False),
           ('sync-menu','',False),('cancel-pending','20261007T120000Z-0001',False),('cancel-all','',False),('usage','half',False),
           ('settings-refresh','',False),('settings-scroll','',False),('notes-long','19',True),
           ('notes-long-last','19',True),('refresh-interval','',False),('refresh-full-only','',False),
           ('storage-info','Insert a FAT32 card.\nOpen Storage to retry.',False),
           ('idle',transcript,True),('live',recent,True),('live-offline',recent,False),
           ('offline',transcript,False),('empty','',False),('notes-empty','',False),
           ('notes-last','',False),('reader',recent,True),('sync','',True),
           ('error','Cannot reach the server. Retry from Settings.',False),('boot','',False),
           ('boot-wifi-pending','',False),('boot-wifi-home-connecting','',False),
           ('boot-wifi-hotspot-connecting','',False),
           ('boot-wifi-home-connected','192.168.1.42',True),
           ('boot-wifi-hotspot-connected','192.168.43.42',True),
           ('boot-wifi-failed','',False),('boot-wifi-disabled','',False),
           ('boot-sd-ready','',False),('boot-sd-failed','',False),('boot-audio','',False),
           ('boot-server-checking','',True),('boot-server-ready','',True),
           ('boot-server-failed','',True),('boot-server-skipped','',False),
           ('sync-cancellable','',True),('sync-stopping','',True),
           ('sync-cancelled','Pending audio kept.\nServer work may finish.\nUse Sync now to retry.',True)]
    if boot_only: cases=[case for case in cases if case[0].startswith('boot') or case[0] in ('sync-cancellable','sync-stopping','sync-cancelled')]
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    reference_frames=[]
    for name,text,wifi in cases:
        mode={'live-offline':'live','empty':'idle','home':'idle','storage-info':'info'}.get(name,name)
        pbm=renderer.path/(name+'.pbm')
        note='' if name in ('home','empty') else '20261006_001'
        if name=='settings-no-sd': note='3'
        elif name=='submenu': note='6'
        elif name=='settings-refresh': mode='settings-no-sd'; note='4'
        elif name=='settings-scroll': mode='settings-no-sd'; note='6'
        elif name in ('notes-long','notes-long-last'): mode='list-notes'; note='0' if name=='notes-long' else '19'
        elif name in ('wifi-menu','storage-menu','sync-menu','cancel-pending','cancel-all','usage'): note='0'
        elif name=='refresh-interval': mode='refresh'; note='10'
        elif name=='refresh-full-only': mode='refresh'; note='0'
        elif name=='storage-info': note='Storage'
        elif name=='error': note='Server unavailable'
        elif name=='sync-cancellable': mode='sync-preview'
        elif name=='sync-cancelled': note='Sync cancelled'
        if name.startswith('boot-wifi-'):
            mode='boot-wifi'
            note={'boot-wifi-pending':'0,0','boot-wifi-home-connecting':'1,0',
                  'boot-wifi-hotspot-connecting':'3,1','boot-wifi-home-connected':'2,4',
                  'boot-wifi-hotspot-connected':'3,2','boot-wifi-failed':'3,3',
                  'boot-wifi-disabled':'5,5'}[name]
        _,meta=renderer.render(mode,text,note=note,wifi=wifi,output=pbm)
        assert meta==(0,1), (name,meta)
        pixels=pbm.read_bytes().split(b'\n',2)[2]
        if name in ('home','notes','settings','sd-error'): reference_frames.append(pixels)
        data=b''.join(b'\0'+bytes(b^255 for b in pixels[y*25:(y+1)*25]) for y in range(200))
        png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',200,200,1,0,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b'')
        (destination/(name+'.png')).write_bytes(png)
        print(destination/(name+'.png'))

    if boot_only: return

    # Pixel-perfect enlargement of actual firmware frames, not a separate mockup.
    gap,scale=16,3
    side=400+3*gap
    rows=[]
    for y in range(side):
        row=bytearray([240]*side)
        for index,frame in enumerate(reference_frames):
            left=gap+(index%2)*(200+gap)
            top=gap+(index//2)*(200+gap)
            if top<=y<top+200:
                for x in range(200):
                    row[left+x]=0 if frame[(y-top)*25+x//8] & (0x80>>(x%8)) else 255
        expanded=b'\0'+b''.join(bytes([pixel])*scale for pixel in row)
        rows.extend([expanded]*scale)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',side*scale,side*scale,8,0,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows)))+chunk(b'IEND',b'')
    (destination/'overview.png').write_bytes(png)
    print(destination/'overview.png')

if __name__=='__main__':
    if len(sys.argv)==3 and sys.argv[1] in ('--preview','--preview-boot'): previews(sys.argv[2],sys.argv[1]=='--preview-boot')
    else: unittest.main()
