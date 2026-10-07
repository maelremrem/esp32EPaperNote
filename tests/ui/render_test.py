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
    bool wifi=std::string(argv[4])=="1";
    if(mode=="idle") ui.showIdle(id,text,0,wifi);
    if(mode=="pending") ui.showIdle(id,text,123456789,wifi);
    if(mode=="live") ui.showLiveRecording(id, text, wifi);
    if(mode=="recording") ui.showRecording(id);
    if(mode=="offline") ui.showOffline(123456789,id,text);
    if(mode=="offline-empty") ui.showOffline(0,"","");
    if(mode=="sync") ui.showSyncing(999,3,id);
    if(mode=="sync-huge") ui.showSyncing(static_cast<size_t>(-1),1,id);
    if(mode=="sync-zero") ui.showSyncing(0,0,id);
    if(mode=="menu") ui.showMenu(2);
    if(mode=="refresh") ui.showRefreshInterval(std::stoul(id));
    if(mode=="settings") {ui.setStatus(wifi,true); ui.showMenu(1);}
    if(mode=="settings-no-sd") {ui.setStatus(wifi,false); ui.showMenu(std::stoul(id));}
    if(mode=="sd-error") ui.showStorageError();
    if(mode=="notes" || mode=="notes-last" || mode=="notes-huge") {
        std::vector<display::SavedNote> notes;
        for(int i=0;i<8;++i) notes.push_back({"Note 0"+std::to_string(i+1),i%2==1});
        ui.showSavedNotes(notes,mode=="notes" ? 0 : (mode=="notes-last" ? 8 : static_cast<size_t>(-1)));
    }
    if(mode=="notes-empty") ui.showSavedNotes({},0);
    if(mode=="reader") ui.showNote(id,text,1);
    if(mode=="reader-huge") ui.showNote(id,text,static_cast<size_t>(-1));
    if(mode=="info") ui.showInfo(id,text);
    if(mode=="error") ui.showError(id,text);
    if(mode=="error-fatal") ui.showError(id,text,false);
    if(mode=="boot") ui.showBoot();
    if(mode=="boot-wifi") {
        auto split=id.find(',');
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
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('UI_TEST_CXXFLAGS','')),'-I'+str(self.path),'-I'+str(ROOT/'firmware/src'),str(self.path/'render.cpp'),str(DISPLAY/'ui.cpp'),'-o',str(self.path/'render')],check=True)

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
        for label in ['Settings','Notes','Wi-Fi','Sync','Storage','About','Back','Next','Select']:
            self.assertIn(label,words)
        self.assertNotIn('Luminosite',words)
        pixels=output.read_bytes().split(b'\n',2)[2]
        self.assertTrue(pixels[55*25+10//8] & (0x80>>(10%8)))
        self.assertEqual(meta,(0,1))

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
            entries=['Notes','Wi-Fi','Sync','Storage','About','Back','Full refresh']
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
            'sync':['Syncing','Sending to transcriber','Wait'],
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

    def test_all_screens_bound_text_before_implicit_wrap(self):
        for mode in ['boot','idle','pending','recording','live','offline','sync','menu','settings','sd-error','error','notes','notes-last','notes-empty','reader','reader-huge','info']:
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
        for mode in ['idle','pending','live','offline','error','reader','info','notes','notes-last','settings']:
            for sample in samples:
                traces,meta=self.renderer.render(mode,sample,note='identifiant'*8)
                self.assertEqual(meta,(0,1))
                boxes=[(x,y,x+len(t)*8*s,y+12*s) for x,y,s,t in traces if t]
                for i,a in enumerate(boxes):
                    for b in boxes[i+1:]:
                        self.assertFalse(a[0]<b[2] and b[0]<a[2] and a[1]<b[3] and b[1]<a[3],(mode,a,b))

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

def previews(destination):
    destination=Path(destination)
    destination.mkdir(parents=True,exist_ok=True)
    renderer=Renderer()
    transcript='Remember to book the train for Friday.\nCall Julie tomorrow to prepare the meeting and share the project notes.'
    recent='\n'.join('Earlier line '+str(i) for i in range(7))+'\nThe latest point:\nprepare the demo and\nconfirm with Julie.'
    cases=[('home','',True),('notes','',True),('settings','',True),('sd-error','',False),
           ('menu','',False),('settings-no-sd','',False),('recording','',False),
           ('settings-refresh','',False),('refresh-interval','',False),('refresh-full-only','',False),
           ('storage-info','Insert a FAT32 card.\nOpen Storage to retry.',False),
           ('idle',transcript,True),('live',recent,True),('live-offline',recent,False),
           ('offline',transcript,False),('empty','',False),('notes-empty','',False),
           ('notes-last','',False),('reader',recent,True),('sync','',True),
           ('error','Cannot reach the server. Retry from Settings.',False),('boot','',False),
           ('boot-wifi-pending','',False),('boot-wifi-home-connecting','',False),
           ('boot-wifi-hotspot-connecting','',False),
           ('boot-wifi-home-connected','192.168.1.42',True),
           ('boot-wifi-hotspot-connected','192.168.43.42',True),
           ('boot-wifi-failed','',False),('boot-wifi-disabled','',False)]
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    reference_frames=[]
    for name,text,wifi in cases:
        mode={'live-offline':'live','empty':'idle','home':'idle','storage-info':'info'}.get(name,name)
        pbm=renderer.path/(name+'.pbm')
        note='' if name in ('home','empty') else '20261006_001'
        if name=='settings-no-sd': note='3'
        elif name=='settings-refresh': mode='settings-no-sd'; note='6'
        elif name=='refresh-interval': mode='refresh'; note='10'
        elif name=='refresh-full-only': mode='refresh'; note='0'
        elif name=='storage-info': note='Storage'
        elif name=='error': note='Server unavailable'
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
    if len(sys.argv)==3 and sys.argv[1]=='--preview': previews(sys.argv[2])
    else: unittest.main()
