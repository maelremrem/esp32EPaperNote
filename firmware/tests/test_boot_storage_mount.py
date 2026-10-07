#!/usr/bin/env python3
"""Real NoteStore SD lifecycle with mocked IDF mount and scratch filesystem."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SD = r'''#pragma once
#include <cstdio>
#include <cassert>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, SDMMC_HOST_SLOT_1=1, SDMMC_SLOT_FLAG_INTERNAL_PULLUP=1;
struct sdmmc_host_t { int slot; };
struct sdmmc_slot_config_t { int width, clk, cmd, d0, flags; };
struct sdmmc_card_t {};
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed; int max_files, allocation_unit_size; };
inline sdmmc_host_t SDMMC_HOST_DEFAULT() { return {}; }
inline sdmmc_slot_config_t SDMMC_SLOT_CONFIG_DEFAULT() { return {}; }
inline bool mount_ok=false, unmount_ok=true;
inline int mounts=0, unmounts=0, live_mounts=0;
inline sdmmc_card_t card;
inline int usage_error=0;
inline uint64_t usage_total=1000, usage_free=500;
inline int esp_vfs_fat_info(const char*, uint64_t* t, uint64_t* f) { *t=usage_total; *f=usage_free; return usage_error; }
inline int formats=0, probes=0;
inline bool format_ok=true, probe_ok=true;
inline int sdmmc_get_status(sdmmc_card_t* value) { assert(value==&card); ++probes; return probe_ok ? ESP_OK : -1; }
inline int esp_vfs_fat_sdcard_format(const char*, sdmmc_card_t* value) {
    assert(value==&card && live_mounts==1); ++formats;
    return format_ok ? ESP_OK : -1;
}
inline int esp_vfs_fat_sdmmc_mount(const char*, const sdmmc_host_t*, const sdmmc_slot_config_t* slot,
 const esp_vfs_fat_sdmmc_mount_config_t* config, sdmmc_card_t** out) {
    ++mounts; assert(!config->format_if_mount_failed);
    assert(slot->width==1 && slot->clk==39 && slot->cmd==41 && slot->d0==40);
    assert(live_mounts==0); // Catch duplicate registration/resource leak.
    if (!mount_ok) { *out=&card; return -1; } // IDF can set out before a late failure.
    *out=&card; ++live_mounts; return ESP_OK;
}
inline int esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t* value) {
    ++unmounts; assert(value==&card && live_mounts==1);
    // IDF consumes/frees the card before unregister-path can return an error.
    --live_mounts;
    return unmount_ok ? ESP_OK : -1;
}
inline const char* esp_err_to_name(int) { return "mock SD error"; }
inline void sdmmc_card_print_info(FILE*, sdmmc_card_t*) {}
'''
TEST = r'''
#include <filesystem>
#include <fstream>
#include <iostream>
#include "storage/note_store.h"
#include "project_config.h"
#include "driver/sdmmc_host.h"
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while(0)
int main(int argc, char** argv) {
    CHECK(argc == 2);
    storage::NoteStore store;
    const std::string scenario=argv[1];
    std::filesystem::create_directories(config::SD_MOUNT_POINT);
    if (scenario=="format") {
        CHECK(!store.format() && formats==0); // Unmounted is never a format request.
        mount_ok=true; CHECK(store.init());
        CHECK(store.format() && formats==1 && probes==1 && live_mounts==1);
        CHECK(std::filesystem::is_directory(config::NOTES_DIR));
    } else if (scenario=="format-fails" || scenario=="format-removed" || scenario=="format-dir-fails" || scenario=="format-cleanup-fails") {
        mount_ok=true; CHECK(store.init());
        format_ok=scenario!="format-fails" && scenario!="format-cleanup-fails";
        probe_ok=scenario!="format-removed";
        unmount_ok=scenario!="format-cleanup-fails";
        if (scenario=="format-dir-fails") {
            std::filesystem::remove_all(config::NOTES_DIR);
            std::ofstream(config::NOTES_DIR) << "obstacle";
        }
        CHECK(!store.format());
        CHECK(formats==(probe_ok ? 1 : 0) && unmounts==1 && live_mounts==0);
        CHECK(store.pendingCount()==0 && !store.commitRecording("unsafe"));
        CHECK(!store.format() && unmounts==1);
        if (!unmount_ok) CHECK(!store.init() && mounts==1);
    } else if (scenario=="retry") {
        CHECK(!store.init() && mounts==1 && live_mounts==0);
        CHECK(store.lastError().find("mount")!=std::string::npos);
        CHECK(store.lastError().find("mock SD error")!=std::string::npos);
        mount_ok=true;
        CHECK(store.init() && mounts==2 && live_mounts==1);
        CHECK(store.lastError().empty());
        std::ofstream(store.recordingTempPath()) << "retained audio";
        CHECK(store.init() && mounts==2 && unmounts==0);
        std::ifstream file(store.recordingTempPath()); std::string content; std::getline(file,content);
        CHECK(content=="retained audio");
    } else if (scenario=="cleanup" || scenario=="cleanup-fails") {
        mount_ok=true;
        // A file occupying an expected directory is not a usable mount.
        std::ofstream(std::string(config::SD_MOUNT_POINT)+"/audio") << "obstacle";
        unmount_ok=scenario!="cleanup-fails";
        CHECK(!store.init());
        CHECK(unmounts==1 && live_mounts==0);
        if (!unmount_ok) {
            CHECK(!store.init() && mounts==1 && unmounts==1);
            std::cout << "PASS storage cleanup-fails safely requires reboot\n";
            return 0;
        }
        std::filesystem::remove(std::string(config::SD_MOUNT_POINT)+"/audio");
        CHECK(store.init() && mounts==2 && live_mounts==1);
        CHECK(store.init() && mounts==2);
    } else if (scenario=="absent-writes") {
        // Even pre-existing paths must not be used without a mounted SD.
        std::filesystem::create_directories(config::RECORDING_DIR);
        std::filesystem::create_directories(config::PENDING_DIR);
        std::filesystem::create_directories(config::ARCHIVE_DIR);
        std::filesystem::create_directories(config::NOTES_DIR);
        std::ofstream(store.recordingTempPath()) << "keep";
        std::ofstream(store.pendingAudioPath("keep")) << "keep";
        CHECK(!store.writeTranscript("new", "text", "fr", 1, "whistle"));
        CHECK(!store.commitRecording("new"));
        CHECK(!store.discardRecording());
        CHECK(!store.archiveAudio("keep"));
        CHECK(std::filesystem::exists(store.recordingTempPath()));
        CHECK(std::filesystem::exists(store.pendingAudioPath("keep")));
        CHECK(!std::filesystem::exists(store.markdownPath("new")));
        CHECK(store.pendingCount()==0 && store.savedNotes().empty());
    } else return 2;
    std::cout << "PASS storage " << scenario << "\n";
}
'''

def main():
    scratch = Path(os.environ.get('TMPDIR', Path.home()/'.hermes/cache/scratch'))
    with tempfile.TemporaryDirectory(prefix='sd-mount-tests-', dir=scratch) as tmp:
        d=Path(tmp)
        (d/'driver').mkdir()
        headers = {
            'driver/sdmmc_host.h': SD,
            'esp_vfs_fat.h': '#include "driver/sdmmc_host.h"\n',
            'sdmmc_cmd.h': '#include "driver/sdmmc_host.h"\n',
            'board_pins.h': '#pragma once\nnamespace board { constexpr int SD_CLK=39, SD_CMD=41, SD_D0=40; }\n',
            'esp_log.h': '#pragma once\n#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)\n',
            'esp_timer.h': '#pragma once\n#include <cstdint>\ninline int64_t esp_timer_get_time() { return 0; }\n',
            'project_config.h': (ROOT/'firmware/include/project_config.h').read_text().replace('/sdcard', str(d/'sdcard')),
        }
        for name, text in headers.items(): (d/name).write_text(text)
        (d/'test.cpp').write_text(TEST)
        subprocess.run(shlex.split(os.environ.get('CXX','g++')) + ['-std=c++17','-Wall','-Wextra','-Werror',
            *shlex.split(os.environ.get('HOST_TEST_FLAGS','')), '-I'+str(d), '-I'+str(ROOT/'firmware/src'),
            str(d/'test.cpp'), str(ROOT/'firmware/src/storage/note_store.cpp'), '-o', str(d/'test')], check=True)
        for scenario in ('format','format-fails','format-removed','format-dir-fails','format-cleanup-fails','retry','cleanup','cleanup-fails','absent-writes'):
            import shutil
            shutil.rmtree(d/'sdcard', ignore_errors=True)
            subprocess.run([str(d/'test'), scenario],check=True)
        # Run the existing real-filesystem history scenarios with a valid mounted
        # fixture, without changing the separately owned legacy runner/tests.
        history = (ROOT/'firmware/tests/note_history_test.cpp').read_text()
        history = '#include "driver/sdmmc_host.h"\n' + history.replace(
            'int main() {', 'int main() {\n    mount_ok=true;\n'
            '    fs::create_directories(config::SD_MOUNT_POINT);\n    CHECK(store.init());')
        (d/'history.cpp').write_text(history)
        shutil.rmtree(d/'sdcard', ignore_errors=True)
        subprocess.run(shlex.split(os.environ.get('CXX','g++')) + ['-std=c++17','-Wall','-Wextra','-Werror',
            *shlex.split(os.environ.get('HOST_TEST_FLAGS','')), '-I'+str(d), '-I'+str(ROOT/'firmware/src'),
            str(d/'history.cpp'), str(ROOT/'firmware/src/storage/note_store.cpp'), '-o', str(d/'history')], check=True)
        subprocess.run([str(d/'history')], check=True)

if __name__=='__main__': main()
