#!/usr/bin/env python3
"""Execute real main.cpp with hardware adapters; no device, secrets or flashing."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TEST = r'''
#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << "\n"; return 1; } } while (0)
int main() {
    store.mount_ok = false;
    int events = 0;
    queue_hook = [&]() {
        if (events == 2) throw std::runtime_error("end after queued navigation");
        *static_cast<app::ButtonEvent *>(mock_event_destination) = events++ == 0
            ? app::ButtonEvent::LongPress : app::ButtonEvent::ShortPress;
        return pdTRUE;
    };
    bool loop_reached = false;
    try { app_main(); } catch (const std::runtime_error &) { loop_reached = true; }
    CHECK(loop_reached && events == 2);
    CHECK(state == AppState::Menu && menu_index == 1 && ui.screen == "settings");
    menu_index = 5; executeMenuItem();
    queue_hook = {};
    CHECK(!sd_mounted && ui.screen == "idle");
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(state == AppState::Idle && recorder.starts == 0 && active_note_id.empty());
    web.queued = true; web.command = network::web::Command::Start; handleWebCommand();
    CHECK(web.result == "rejected" && recorder.starts == 0);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(state == AppState::Menu && ui.screen == "settings");
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(menu_index == 1 && ui.selected == 1);
    menu_index = 3; executeMenuItem();
    CHECK(!sd_mounted && ui.screen == "info");
    store.mount_ok = true;
    handleButton(app::ButtonEvent::LongPress); // Back from info.
    handleButton(app::ButtonEvent::LongPress); // Retry Storage after insertion.
    CHECK(sd_mounted && state == AppState::Menu && ui.screen == "info");
    CHECK(store.writes == 0 && store.discards == 0 && store.commits == 0);
    handleButton(app::ButtonEvent::LongPress); menu_index = 5; executeMenuItem();
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(recorder.starts == 1 && state == AppState::Recording);
    handleButton(app::ButtonEvent::ShortPress); recorder.clean = false; pollRecording();
    CHECK(recording_recovery_required && store.discards == 0);
    handleButton(app::ButtonEvent::LongPress); menu_index = 3; executeMenuItem();
    CHECK(recording_recovery_required); // Retry must never clear retained-WAV protection.
    handleButton(app::ButtonEvent::LongPress); menu_index = 5; executeMenuItem();
    handleButton(app::ButtonEvent::ShortPress); CHECK(recorder.starts == 1);
    state = AppState::Idle; recording_recovery_required = false;
    recorder = audio::AudioRecorder{}; recorder.init_ok = false;
    loop_reached = false;
    try { app_main(); } catch (const std::runtime_error &) { loop_reached = true; }
    CHECK(loop_reached && sd_mounted && ui.screen == "idle");
    handleButton(app::ButtonEvent::ShortPress);
    CHECK(state == AppState::Idle && recorder.starts == 0);
    handleButton(app::ButtonEvent::LongPress);
    CHECK(state == AppState::Menu && ui.screen == "settings");
    handleButton(app::ButtonEvent::ShortPress); CHECK(menu_index == 1);
    CHECK(ui.renders_without_status == 0);
    std::cout << "PASS real-main boot without SD, Storage retry, recording guards, audio-failure navigation\n";
}
'''

def main():
    source = re.sub(r'^#include[^\n]*$', '', (ROOT / 'firmware/src/main.cpp').read_text(), flags=re.MULTILINE)
    scratch = Path(os.environ.get('TMPDIR', Path.home() / '.hermes/cache/scratch'))
    with tempfile.TemporaryDirectory(prefix='boot-storage-', dir=scratch) as tmp:
        d = Path(tmp)
        mocks = (ROOT/'firmware/tests/stubs/navigation_mocks.h').read_text().replace(
            'inline int xQueueReceive(int, void *, int) {',
            'inline void *mock_event_destination = nullptr;\n'
            'inline int xQueueReceive(int, void *destination, int) {\n'
            '    mock_event_destination = destination;')
        (d/'navigation_mocks.h').write_text(mocks)
        (d / 'test.cpp').write_text('#include "navigation_mocks.h"\n' + source + '\n' + TEST)
        command = shlex.split(os.environ.get('CXX', 'g++')) + ['-std=c++17', '-Wall', '-Wextra', '-Wno-unused-variable',
            *shlex.split(os.environ.get('HOST_TEST_FLAGS', '')), '-I'+str(ROOT/'firmware/tests/stubs'),
            '-I'+str(ROOT/'firmware/src'), '-I'+str(ROOT/'firmware/include'), str(d/'test.cpp'), '-o', str(d/'test')]
        subprocess.run(command, check=True)
        subprocess.run([str(d/'test')], check=True)

if __name__ == '__main__':
    main()
