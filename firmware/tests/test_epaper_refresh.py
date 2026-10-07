"""Host transport tests: real display driver, only ESP GPIO/SPI/time are stubbed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

FULL_LUT = bytes.fromhex("8048400000000000000000004048800000000000000000008048400000000000000000004048800000000000000000000000000000000000000000000a000000000000080100080100020a000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000222222222222000000221741003220")
PARTIAL_LUT = bytes.fromhex("0040000000000000000000008080000000000000000000004040000000000000000000000080000000000000000000000000000000000000000000000f0000000000000101000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000222222222222000000021741b03228")

STUB = r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=2, GPIO_NUM_NC=-1, SPI_DMA_CH_AUTO=0;
constexpr int GPIO_MODE_OUTPUT=1, GPIO_MODE_INPUT=0, GPIO_PULLUP_DISABLE=0, GPIO_PULLDOWN_DISABLE=0, GPIO_INTR_DISABLE=0;
using spi_device_handle_t=void*;
struct gpio_config_t {uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type;};
struct spi_bus_config_t {int mosi_io_num,miso_io_num,sclk_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz;};
struct spi_device_interface_config_t {int mode,clock_speed_hz,spics_io_num,queue_size;};
struct spi_transaction_t {size_t length; const void* tx_buffer;};
int gpio_config(const gpio_config_t*);
int gpio_set_level(int,int);
int gpio_get_level(int);
int spi_bus_initialize(int,const spi_bus_config_t*,int);
int spi_bus_add_device(int,const spi_device_interface_config_t*,void**);
int spi_device_polling_transmit(void*,spi_transaction_t*);
int64_t esp_timer_get_time();
void vTaskDelay(int);
inline const char* esp_err_to_name(int) {return "injected error";}
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
'''
HARNESS = r'''
#include "stub.h"
#include "display/epaper_display.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
int levels[64]{};
int64_t now=0;
int calls=0, fail_at=-1;
bool busy=false;
int busy_command=-1,busy_activation=-1,activations=0;
struct Packet {int command; std::vector<unsigned char> data;};
std::vector<Packet> packets;
std::vector<std::pair<int64_t,int>> reset_events;
int gpio_config(const gpio_config_t*) {return 0;}
int gpio_set_level(int p,int v) {levels[p]=v;if(p==9) reset_events.push_back({now,v});return 0;}
int gpio_get_level(int) {return busy;}
int spi_bus_initialize(int,const spi_bus_config_t*,int) {return 0;}
int spi_bus_add_device(int,const spi_device_interface_config_t*,void** p) {*p=reinterpret_cast<void*>(1);return 0;}
int spi_device_polling_transmit(void*,spi_transaction_t* t) {
    ++calls;
    if(calls==fail_at) return 1;
    auto p=static_cast<const unsigned char*>(t->tx_buffer);
    if(levels[10]==0) {packets.push_back({p[0],{}});
        if(p[0]==busy_command || (p[0]==0x20 && ++activations==busy_activation)) busy=true;
    }
    else {if(packets.empty()) packets.push_back({0xff,{}}); packets.back().data.insert(packets.back().data.end(),p,p+t->length/8);}
    return 0;
}
int64_t esp_timer_get_time() {return now;}
void vTaskDelay(int ms) {now+=int64_t(ms)*1000;}
void dump(const char* phase) {
    std::cout<<phase<<"\n";
    for(auto &p:packets) {std::cout<<std::hex<<p.command<<":";
        for(auto b:p.data) std::cout<<std::setw(2)<<std::setfill('0')<<int(b);
        std::cout<<"\n";}
    packets.clear();
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    std::string mode=argv[1];
    display::EpaperDisplay d;
    if(mode=="init-spi" || mode=="init-timeout") {
        if(mode=="init-spi") fail_at=1;else busy=true;
        std::cout<<(d.init()?"INIT_SUCCESS":"INIT_FAILURE")<<"\n";
        dump("INIT");fail_at=-1;busy=false;
        d.clear();if(!d.refresh(false)) return 7;dump("RECOVERY");return 0;
    }
    if(!d.init()) return 3;
    d.drawPixel(0,0); d.drawPixel(199,199); d.refresh(); dump("BASE");
    if(mode=="base") return 0;
    if(mode.rfind("limit-",0)==0) {
        int limit=std::stoi(mode.substr(6));
        d.setPartialRefreshLimit(limit);
        for(int i=1;i<=limit+2;++i) {d.drawPixel(i,3);d.refresh();dump(("LIMIT"+std::to_string(i)).c_str());}
        return 0;
    }
    if(mode=="lower-limit") {
        for(int i=0;i<5;++i) {d.drawPixel(i,4);d.refresh();}
        dump("DISCARD");d.setPartialRefreshLimit(1);
        d.drawPixel(8,4);d.refresh();dump("LOWER");return 0;
    }
    reset_events.clear();
    const int part_start=calls;
    d.drawPixel(9,1); d.refresh(); dump("PART");
    std::cout<<"PART_COUNT"<<std::dec<<(calls-part_start)<<"\n";
    if(reset_events.size()!=3 || reset_events[0].second!=1 || reset_events[1].second!=0 || reset_events[2].second!=1 ||
        reset_events[1].first-reset_events[0].first!=50000 || reset_events[2].first-reset_events[1].first!=20000 || now-reset_events[2].first<50000)
        std::cout<<"RESET_FAILURE\n";
    if(mode=="unchanged") {d.refresh(); dump("SAME");}
    if(mode=="force") {
        const int full_start=calls;
        if(!d.refresh(true)) return 4;
        dump("FORCE");std::cout<<"FULL_COUNT"<<std::dec<<(calls-full_start)<<"\n";
        d.drawPixel(10,1);d.refresh();dump("AFTER");
    }
    if(mode.rfind("spi-",0)==0 || mode.rfind("full-spi-",0)==0 || mode.rfind("timeout",0)==0) {
        d.drawPixel(10,1);
        const int start=calls;
        activations=0;
        if(mode=="timeout") busy=true;
        else if(mode=="timeout-lut") busy_command=0x32;
        else if(mode=="timeout-prepare") busy_activation=1;
        else if(mode=="timeout-activate") busy_activation=2;
        else fail_at=calls+std::stoi(mode.substr(mode.rfind("full-",0)==0?9:4));
        const bool ok=d.refresh(mode.rfind("full-",0)==0);
        std::cout<<(ok?"SUCCESS":"FAILURE")<<"\n";
        if(mode.find("spi-")!=std::string::npos && calls>fail_at) std::cout<<"EXCESS_SPI\n";
        if(now>30000000) std::cout<<"EXCESS_TIME\n";
        (void)start;
        dump("BROKEN");busy=false;fail_at=-1;busy_command=-1;busy_activation=-1;
        // Returning to the last visible frame must not skip recovery.
        d.drawPixel(10,1,false);
        if(!d.refresh(false)) return 5;
        dump("RECOVERY");
        d.drawPixel(10,1);d.refresh();dump("AFTER");
    }
    if(mode=="sleep") {
        d.sleep();dump("SLEEP");
        if(levels[6]!=1) std::cout<<"POWER_FAILURE\n";
        if(!d.refresh(false)) return 6;
        if(levels[6]!=0) std::cout<<"POWER_FAILURE\n";
        dump("WAKE");d.drawPixel(10,1);d.refresh();dump("AFTER");
    }
    if(mode=="periodic") {
        for(int i=2;i<=25;++i) {d.drawPixel(i+10,2);d.refresh();dump(("STEP"+std::to_string(i)).c_str());}
    }
}
'''

class RefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='epaper-refresh-', dir=os.environ.get('TMPDIR'))
        cls.path = Path(cls.tmp.name)
        (cls.path/'stub.h').write_text(STUB)
        for name in ('driver/spi_master.h','driver/gpio.h','esp_log.h','esp_timer.h','freertos/task.h'):
            file = cls.path/name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('#include "stub.h"\n')
        (cls.path/'board_pins.h').write_text('namespace board { constexpr int EPD_PWR=6,EPD_RST=9,EPD_DC=10,EPD_CS=11,EPD_BUSY=8,EPD_MOSI=13,EPD_SCLK=12,EPD_SPI_HOST=2; }\n')
        (cls.path/'harness.cpp').write_text(HARNESS)
        subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-Wall','-Wextra','-Werror', *shlex.split(os.environ.get('HOST_TEST_FLAGS','')), '-I'+str(cls.path), '-I'+str(ROOT/'firmware/src'), str(cls.path/'harness.cpp'), str(ROOT/'firmware/src/display/epaper_display.cpp'), '-o', str(cls.path/'test')],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_driver(self, mode):
        phases = {}
        current = None
        for row in subprocess.check_output([str(self.path/'test'),mode],text=True).splitlines():
            if ':' not in row:
                current = phases.setdefault(row, [])
            else:
                command, data = row.split(':')
                assert current is not None
                current.append((int(command,16),bytes.fromhex(data)))
        return phases

    def test_configurable_partial_budget_and_full_only(self):
        for limit in (0,1,5,10,20,50,100):
            phases = self.run_driver('limit-'+str(limit))
            for i in range(1,limit+3):
                full = limit == 0 or i % (limit+1) == 0
                self.assertEqual(phases['LIMIT'+str(i)][-2:], [(0x22,b'\xc7' if full else b'\xcf'),(0x20,b'')])
        self.assertEqual(self.run_driver('lower-limit')['LOWER'][-2:],[(0x22,b'\xc7'),(0x20,b'')])

    def test_first_refresh_seeds_both_planes_with_full_activation(self):
        packets = self.run_driver('base')['BASE']
        ram = [(c,p) for c,p in packets if c in (0x24,0x26)]
        self.assertEqual([c for c,p in ram], [0x24,0x26])
        expected = bytearray(b'\xff'*5000)
        expected[0] = 0x7f
        expected[-1] = 0xfe
        self.assertEqual([p for c,p in ram], [expected,expected])
        self.assertEqual(packets[-2:], [(0x22,b'\xc7'),(0x20,b'')])
        self.assertIn((0x11,b'\x03'),packets)
        self.assertIn((0x45,b'\x00\x00\xc7\x00'),packets)

    def assert_lut(self, packets, lut):
        pairs = ((0x32,lut[:153]),(0x3f,lut[153:154]),(0x03,lut[154:155]),(0x04,lut[155:158]),(0x2c,lut[158:]))
        for pair in pairs:
            self.assertIn(pair,packets)
        indexes = [packets.index(pair) for pair in pairs]
        self.assertEqual(indexes, sorted(indexes))

    def test_full_temperature_preparation_follows_vendor_before_explicit_lut(self):
        packets = self.run_driver('base')['BASE']
        self.assertIn((0x22,b'\xb1'),packets)
        index = packets.index((0x22,b'\xb1'))
        self.assertEqual(packets[index+1],(0x20,b''))
        self.assertLess(index,packets.index((0x32,FULL_LUT[:153])))

    def test_full_loads_exact_vendor_waveform_and_analog_settings(self):
        packets = self.run_driver('base')['BASE']
        self.assert_lut(packets,FULL_LUT)
        self.assertIn((0x3c,b'\x01'),packets)
        self.assertLess(packets.index((0x32,FULL_LUT[:153])),next(i for i,(c,p) in enumerate(packets) if c==0x24))

    def test_changed_frame_uses_vendor_partial_preparation_and_old_reference(self):
        phases = self.run_driver('partial')
        packets = phases['PART']
        self.assert_lut(packets,PARTIAL_LUT)
        self.assertIn((0x37,bytes.fromhex('00000000004000000000')),packets)
        self.assertIn((0x3c,b'\x80'),packets)
        self.assertIn((0x22,b'\xc0'),packets)
        self.assertEqual(packets[-2:],[(0x22,b'\xcf'),(0x20,b'')])
        old = next(p for c,p in phases['BASE'] if c==0x24)
        new = bytearray(old)
        new[26] &= 0xbf
        self.assertEqual([(c,p) for c,p in packets if c in (0x24,0x26)],[(0x26,old),(0x24,new)])
        self.assertLess(packets.index((0x22,b'\xc0')), packets.index((0x24,new)))
        self.assertIn((0x11,b'\x03'),packets)
        self.assertIn((0x45,b'\x00\x00\xc7\x00'),packets)

    def test_identical_frame_does_not_use_spi(self):
        self.assertEqual(self.run_driver('unchanged')['SAME'],[])

    def test_periodic_full_clean_restores_full_settings_and_repeated_references(self):
        phases = self.run_driver('periodic')
        old = next(p for c,p in phases['BASE'] if c==0x24)
        for i in range(1,26):
            packets = phases['PART' if i==1 else 'STEP'+str(i)]
            full = i in (11,22)
            self.assertEqual(packets[-2:],[(0x22,b'\xc7' if full else b'\xcf'),(0x20,b'')])
            new = next(p for c,p in packets if c==0x24)
            self.assertEqual(next(p for c,p in packets if c==0x26),new if full else old)
            if full:
                self.assert_lut(packets,FULL_LUT)
                self.assertIn((0x3c,b'\x01'),packets)
            old = new

    def test_force_full_refreshes_even_identical_frame_and_resets_partial_budget(self):
        phases = self.run_driver('force')
        packets = phases['FORCE']
        self.assert_lut(packets,FULL_LUT)
        self.assertIn((0x3c,b'\x01'),packets)
        self.assertEqual(packets[-2:],[(0x22,b'\xc7'),(0x20,b'')])
        self.assertEqual([c for c,p in packets if c in (0x24,0x26)],[0x24,0x26])
        self.assertEqual(phases['AFTER'][-2:],[(0x22,b'\xcf'),(0x20,b'')])

    def assert_failure_recovers_full(self, phases):
        self.assertIn('FAILURE',phases)
        self.assertNotIn('EXCESS_SPI',phases)
        self.assertNotIn('EXCESS_TIME',phases)
        packets = phases['RECOVERY']
        self.assert_lut(packets,FULL_LUT)
        self.assertEqual(packets[-2:],[(0x22,b'\xc7'),(0x20,b'')])
        old = next(p for c,p in phases['PART'] if c==0x24)
        self.assertEqual([p for c,p in packets if c in (0x24,0x26)],[old,old])
        self.assertEqual(next(p for c,p in phases['AFTER'] if c==0x26),old)

    def test_spi_failure_invalidates_reference_and_stops_current_transfer(self):
        phases = self.run_driver('partial')
        count = int(next(k.removeprefix('PART_COUNT') for k in phases if k.startswith('PART_COUNT')))
        for n in range(1,count+1):
            with self.subTest(transaction=n):
                self.assert_failure_recovers_full(self.run_driver('spi-'+str(n)))

    def test_busy_timeout_invalidates_reference_without_sending_more_commands(self):
        phases = self.run_driver('timeout')
        self.assert_failure_recovers_full(phases)
        self.assertEqual(phases['BROKEN'],[])

    def test_sleep_invalidates_reference_and_wakes_to_full_without_clearing_drawing(self):
        phases = self.run_driver('sleep')
        self.assertEqual(phases['SLEEP'],[(0x10,b'\x01')])
        self.assertNotIn('POWER_FAILURE',phases)
        packets = phases['WAKE']
        self.assert_lut(packets,FULL_LUT)
        self.assertIn((0x12,b''),packets)
        self.assertEqual(packets[-2:],[(0x22,b'\xc7'),(0x20,b'')])
        old = next(p for c,p in phases['PART'] if c==0x24)
        self.assertEqual([p for c,p in packets if c in (0x24,0x26)],[old,old])
        self.assertEqual(phases['AFTER'][-2:],[(0x22,b'\xcf'),(0x20,b'')])

    def test_timeout_at_lut_preparation_or_activation_never_commits_reference(self):
        for mode in ('timeout-lut','timeout-prepare','timeout-activate'):
            with self.subTest(mode=mode):
                self.assert_failure_recovers_full(self.run_driver(mode))

    def test_initialization_reports_spi_or_busy_failure_and_recovers_full(self):
        for mode in ('init-spi','init-timeout'):
            with self.subTest(mode=mode):
                phases = self.run_driver(mode)
                self.assertIn('INIT_FAILURE',list(phases))
                if mode=='init-timeout':
                    self.assertEqual(phases['INIT'],[])
                self.assert_lut(phases['RECOVERY'],FULL_LUT)
                self.assertEqual(phases['RECOVERY'][-2:],[(0x22,b'\xc7'),(0x20,b'')])

    def test_partial_uses_vendor_hardware_reset_and_preserves_ram_without_swreset(self):
        phases = self.run_driver('partial')
        self.assertNotIn('RESET_FAILURE',list(phases))
        self.assertNotIn(0x12,[c for c,p in phases['PART']])

    def test_full_refresh_spi_failures_invalidate_and_recover(self):
        phases = self.run_driver('force')
        count = int(next(k.removeprefix('FULL_COUNT') for k in phases if k.startswith('FULL_COUNT')))
        for n in range(1,count+1):
            with self.subTest(transaction=n):
                self.assert_failure_recovers_full(self.run_driver('full-spi-'+str(n)))

    def test_every_ram_plane_rewinds_cursor_before_transfer(self):
        phases = self.run_driver('partial')
        for phase in ('BASE','PART'):
            packets = phases[phase]
            for index,(command,data) in enumerate(packets):
                if command in (0x24,0x26):
                    self.assertEqual(packets[index-2:index],[(0x4e,b'\x00'),(0x4f,b'\x00\x00')])

if __name__ == '__main__':
    unittest.main()
