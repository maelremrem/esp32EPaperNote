#!/usr/bin/env python3
"""Exercise real button task with a simulated clock/GPIO, not physical hardware."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
pins = (ROOT / 'firmware/include/board_pins.h').read_text()
assert re.search(r'APP_BUTTON\s*=\s*GPIO_NUM_0\s*;', pins), 'Use onboard BOOT on GPIO0'
source = re.sub(r'^#include[^\n]*$', '', (ROOT/'firmware/src/app/button.cpp').read_text(), flags=re.M)
header = re.sub(r'^#(?:include|pragma)[^\n]*$', '', (ROOT/'firmware/src/app/button.h').read_text(), flags=re.M).replace('private:', 'public:')
preamble = r'''
#include <cstdint>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cassert>
#include <iostream>
using QueueHandle_t = void*;
constexpr int ESP_OK=0, GPIO_MODE_INPUT=0, GPIO_PULLUP_ENABLE=1, GPIO_PULLDOWN_DISABLE=0, GPIO_INTR_DISABLE=0, pdPASS=1;
struct gpio_config_t { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; };
namespace board { constexpr int APP_BUTTON=0; }
namespace config { constexpr int BUTTON_DEBOUNCE_MS=35,BUTTON_DOUBLE_MS=350,BUTTON_LONG_MS=800; }
inline int64_t now=0;
inline int64_t end_time=0;
inline std::vector<std::pair<int64_t,int>> levels;
inline std::vector<int> events;
int gpio_get_level(int pin) { assert(pin==0); int level=1; for (auto change:levels) if (now>=change.first) level=change.second; return level; }
int64_t esp_timer_get_time() { return now; }
int gpio_config(gpio_config_t* io) { assert(io->pin_bit_mask==1 && io->pull_up_en==1); return ESP_OK; }
QueueHandle_t xQueueCreate(int,int) { return reinterpret_cast<void*>(1); }
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*) { return pdPASS; }
int xQueueSend(QueueHandle_t,const void* ev,int) { events.push_back(*static_cast<const uint8_t*>(ev)); return 1; }
int pdMS_TO_TICKS(int value) { return value; }
void vTaskDelay(int ms) { now+=ms*1000; if(now>end_time) throw std::runtime_error("end"); }
'''
tests = r'''
void run(std::vector<std::pair<int64_t,int>> changes,int duration,std::vector<int> expected) {
 now=0; levels=changes; events.clear(); end_time=duration*1000; app::Button button;
 assert(button.init()); try { button.task(); } catch(const std::runtime_error&) {}
 if(events!=expected) { std::cerr<<"GPIO timeline starting "<<changes.front().first<<", duration "<<duration<<": got"; for(int ev:events) std::cerr<<" "<<ev; std::cerr<<" expected"; for(int ev:expected) std::cerr<<" "<<ev; std::cerr<<"\n"; }
 assert(events==expected);
}
int main() {
 run({{100000,0},{250000,1}},1000,{int(app::ButtonEvent::ShortPress)});
 run({{100000,0},{250000,1},{350000,0},{500000,1}},1000,{int(app::ButtonEvent::DoublePress)});
 run({{100000,0},{1100000,1}},1600,{int(app::ButtonEvent::LongPress)});
 // Releasing BOOT held at task startup must never create a recording/menu event.
 run({{0,0},{100000,1}},1000,{});
 run({{0,0},{100000,1},{300000,0},{450000,1}},1100,{int(app::ButtonEvent::ShortPress)});
 std::cout<<"PASS GPIO0, debounce, short/double/long, startup-held release\n";
}
'''
scratch = Path(os.environ.get('TMPDIR', Path.home()/'.hermes/cache/scratch'))
with tempfile.TemporaryDirectory(prefix='boot-button-',dir=scratch) as tmp:
    path=Path(tmp)
    (path/'test.cpp').write_text(preamble+header+source+tests)
    subprocess.run(shlex.split(os.environ.get('CXX','g++'))+['-std=c++17','-Wall','-Wextra','-Wno-unused-variable',*shlex.split(os.environ.get('HOST_TEST_FLAGS','')),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
