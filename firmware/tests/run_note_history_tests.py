#!/usr/bin/env python3
"""Run real note_store.cpp against scratch files; adapt only ESP SD mounting.

No credentials, hardware, or /sdcard access. Supports CXX and HOST_TEST_FLAGS.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
scratch = Path(os.environ.get('TMPDIR', str(Path.home() / '.hermes/cache/scratch')))
scratch.mkdir(parents=True, exist_ok=True)
sd = r'''#pragma once
#include <cstdio>
using esp_err_t = int;
constexpr int ESP_OK = 0, SDMMC_HOST_SLOT_1 = 1, SDMMC_SLOT_FLAG_INTERNAL_PULLUP = 1;
struct sdmmc_host_t { int slot; };
struct sdmmc_slot_config_t { int width, clk, cmd, d0, flags; };
struct sdmmc_card_t {};
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed; int max_files; int allocation_unit_size; };
inline sdmmc_host_t SDMMC_HOST_DEFAULT() { return {}; }
inline sdmmc_slot_config_t SDMMC_SLOT_CONFIG_DEFAULT() { return {}; }
inline sdmmc_card_t card;
inline int esp_vfs_fat_sdmmc_mount(const char*, const sdmmc_host_t*, const sdmmc_slot_config_t*, const esp_vfs_fat_sdmmc_mount_config_t*, sdmmc_card_t** out) { *out = &card; return ESP_OK; }
inline int esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t*) { return ESP_OK; }
inline const char *esp_err_to_name(int) { return "host: no SD mounting"; }
inline void sdmmc_card_print_info(FILE*, sdmmc_card_t*) {}
'''
with tempfile.TemporaryDirectory(prefix='note-history-tests-', dir=scratch) as directory:
    d = Path(directory)
    (d / 'driver').mkdir()
    headers = {
        'driver/sdmmc_host.h': sd,
        'esp_vfs_fat.h': '#include "driver/sdmmc_host.h"\n',
        'sdmmc_cmd.h': '#include "driver/sdmmc_host.h"\n',
        'board_pins.h': '#pragma once\nnamespace board { constexpr int SD_CLK=0, SD_CMD=0, SD_D0=0; }\n',
        'esp_log.h': '#pragma once\n#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)\n',
        'esp_timer.h': '#pragma once\n#include <cstdint>\ninline int64_t esp_timer_get_time() { return 0; }\n',
        'project_config.h': (root / 'firmware/include/project_config.h').read_text().replace('/sdcard', str(d / 'sdcard')),
    }
    for name, content in headers.items():
        (d / name).write_text(content)
    binary = str(d / 'note_history_test')
    history = (root / 'firmware/tests/note_history_test.cpp').read_text().replace(
        'int main() {', 'int main() {\n    fs::create_directories(config::SD_MOUNT_POINT);\n    CHECK(store.init());')
    (d / 'history.cpp').write_text(history)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    *shlex.split(os.environ.get('HOST_TEST_FLAGS', '')),
                    '-I'+str(d), '-I'+str(root / 'firmware/src'),
                    str(d / 'history.cpp'),
                    str(root / 'firmware/src/storage/note_store.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
