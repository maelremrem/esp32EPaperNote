#!/usr/bin/env python3
"""Host tests of the complete, unmodified main.cpp body with hardware mocks.

Only #includes are replaced. The real handleButton, executeMenuItem, syncPending,
recording lifecycle, web command handler and app_main execute; web_policy.h and
live_preview_helpers.h are real. No ESP-IDF build, flash, secrets or hardware.
Run from any directory; CXX and HOST_TEST_FLAGS customize the host compiler.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = r'''
#define CHECK(condition) do { if (!(condition)) { std::cerr << __func__ << ":" << __LINE__ << ": " << #condition << "\n"; return false; } } while (0)
using Event = app::ButtonEvent;
void reset() {
    state = AppState::Idle;
    menu_index = 0;
    menu_view = MenuView::Settings;
    saved_notes.clear(); reader_text.clear(); info_title.clear(); info_message.clear();
    note_index = reader_page = 0;
    sd_mounted = audio_ready = true; // Normal scenarios have usable peripherals.
    button = app::Button{}; queue_hook = {};
    active_note_id.clear(); last_note_id.clear(); last_note_text.clear(); live_text.clear();
    stopping_recording = recording_start_failed = recording_recovery_required = false;
    store = storage::NoteStore{}; recorder = audio::AudioRecorder{};
    wifi = network::WifiManager{}; web = network::WebServer{};
    preview = network::LivePreview{}; api = network::ApiClient{};
    ui = display::Ui(epaper); display::Ui::pages = 3;
}
bool gestures() {
    for (auto start : {Event::ShortPress, Event::DoublePress}) {
        for (auto stop : {Event::ShortPress, Event::DoublePress, Event::LongPress}) {
            reset();
            handleButton(start);
            CHECK(state == AppState::Recording && recorder.starts == 1);
            handleButton(stop);
            CHECK(stopping_recording && recorder.stops == 1 && preview.stops == 1);
        }
    }
    reset(); store.pending = {"offline-note"};
    handleButton(Event::LongPress);
    CHECK(state == AppState::Menu && ui.screen == "settings" && ui.selected == 0);
    CHECK(wifi.connects == 0 && api.calls == 0);
    for (size_t row = 1; row < display::Ui::MENU_ITEMS; ++row) {
        handleButton(row % 2 ? Event::ShortPress : Event::DoublePress);
        CHECK(ui.selected == row && state == AppState::Menu);
    }
    handleButton(Event::ShortPress);
    CHECK(ui.selected == 0);
    return true;
}
bool history_reader() {
    reset();
    store.history = {{"note-new", true}, {"audio-only", false}, {"note-old", true}};
    handleButton(Event::LongPress); // Settings.
    handleButton(Event::LongPress); // Notes.
    CHECK(state == AppState::Menu && ui.screen == "notes" && ui.selected == 0);
    CHECK(ui.notes.size() == 3 && store.requested_limit > 0 && store.requested_limit <= 100);
    handleButton(Event::LongPress);
    CHECK(state == AppState::Menu && ui.screen == "reader" && ui.note_id == "note-new");
    CHECK(store.read_id == "note-new" && ui.message == store.transcript && ui.page == 0);
    handleButton(Event::ShortPress); CHECK(ui.page == 1);
    handleButton(Event::DoublePress); CHECK(ui.page == 2);
    handleButton(Event::ShortPress); CHECK(ui.page == 0);
    CHECK(store.reads == 1); // Pagination must use the cached text.
    handleButton(Event::LongPress); CHECK(ui.screen == "notes" && ui.selected == 0);
    handleButton(Event::DoublePress); handleButton(Event::LongPress);
    CHECK(ui.screen == "reader" && ui.note_id == "audio-only" && store.reads == 1);
    CHECK(ui.message.find("Transcription unavailable") != std::string::npos);
    CHECK(ui.message.find("audio playback") == std::string::npos);
    handleButton(Event::LongPress); handleButton(Event::ShortPress);
    CHECK(ui.selected == 2);
    store.read_ok = false;
    handleButton(Event::LongPress);
    CHECK(ui.screen == "reader" && ui.message.find("Cannot read") != std::string::npos);
    handleButton(Event::LongPress); handleButton(Event::DoublePress);
    CHECK(ui.screen == "notes" && ui.selected == ui.notes.size());
    handleButton(Event::ShortPress); CHECK(ui.selected == 0); // Includes Return in the cycle.
    for (int i = 0; i < 3; ++i) handleButton(Event::ShortPress);
    handleButton(Event::LongPress); CHECK(ui.screen == "settings" && state == AppState::Menu);
    for (int i = 0; i < 5; ++i) handleButton(Event::ShortPress);
    handleButton(Event::LongPress); CHECK(state == AppState::Idle);
    CHECK(saved_notes.empty() && saved_notes.capacity() == 0 && reader_text.empty());
    store.history.clear();
    handleButton(Event::LongPress); handleButton(Event::LongPress);
    CHECK(ui.screen == "notes" && ui.notes.empty() && ui.selected == 0);
    handleButton(Event::DoublePress); CHECK(ui.selected == 0);
    handleButton(Event::LongPress); CHECK(ui.screen == "settings");
    for (int i = 0; i < 140; ++i) store.history.push_back({"n" + std::to_string(i), true});
    handleButton(Event::LongPress);
    CHECK(ui.notes.size() == store.requested_limit && ui.notes.size() <= 100);
    return true;
}
void open_row(size_t row) {
    handleButton(Event::LongPress);
    for (size_t i = 0; i < row; ++i) handleButton(Event::ShortPress);
    handleButton(Event::LongPress);
}
bool settings_actions() {
    reset(); open_row(1);
    CHECK(state == AppState::Menu && ui.screen == "info" && wifi.connects == 1);
    CHECK(ui.message.find(wifi.currentSsid()) != std::string::npos);
    CHECK(ui.message.find(wifi.ipAddress()) != std::string::npos);
    handleButton(Event::ShortPress); CHECK(ui.screen == "info");
    handleButton(Event::LongPress); CHECK(ui.screen == "settings" && ui.selected == 1);
    reset(); wifi.connect_ok = false; open_row(1);
    CHECK(ui.screen == "info" && ui.message.find("unavailable") != std::string::npos);
    reset(); wifi.connect_ok = false; store.pending = {"p"}; open_row(2);
    CHECK(state == AppState::Menu && ui.screen == "info" && wifi.connects == 1 && api.calls == 0);
    CHECK(ui.message.find("network") != std::string::npos);
    reset(); open_row(2);
    CHECK(ui.screen == "info" && ui.message.find("No pending") != std::string::npos && api.calls == 0);
    reset(); store.pending = {"p"}; open_row(2);
    CHECK(state == AppState::Menu && ui.screen == "info" && api.calls == 1);
    CHECK(api.path == "/sdcard/pending/p.wav" && store.writes == 1 && store.archives == 1);
    CHECK(ui.message.find("complete") != std::string::npos);
    bool saw_sync = false;
    for (const auto &s : web.snapshots) { CHECK(s.state == AppState::Menu || s.state == AppState::Syncing); saw_sync |= s.state == AppState::Syncing; }
    CHECK(saw_sync && web.snapshots.back().state == AppState::Menu);
    reset(); wifi.online = true; api.ok = false; store.pending = {"p"}; open_row(2);
    CHECK(state == AppState::Menu && wifi.connects == 0 && store.archives == 0);
    CHECK(ui.message.find("Failed") != std::string::npos && store.pendingCount() == 1);
    reset(); store.write_ok = false; store.pending = {"p"}; open_row(2);
    CHECK(ui.message.find("Failed") != std::string::npos && store.archives == 0);
    reset(); store.archive_ok = false; store.pending = {"p"}; open_row(2);
    CHECK(ui.message.find("1") != std::string::npos && store.pendingCount() == 1);
    reset(); store.pending = {"a", "b"}; open_row(3);
    CHECK(state == AppState::Menu && ui.screen == "info");
    CHECK(ui.message.find("SD") != std::string::npos && ui.message.find("2") != std::string::npos);
    CHECK(ui.message.find("Go") == std::string::npos && ui.message.find("Mo") == std::string::npos);
    reset(); open_row(4);
    CHECK(ui.screen == "info" && ui.message.find("ESP32-S3") != std::string::npos);
    CHECK(ui.message.find("200x200") != std::string::npos && ui.message.find("Whistle") != std::string::npos);
    return true;
}
bool status_and_startup() {
    reset(); store.mount_ok = false;
    try { app_main(); } catch (const std::runtime_error &) {}
    CHECK(ui.screen == "idle" && !ui.sd_status && !sd_mounted);
    CHECK(ui.renders_without_status == 0);
    reset(); recorder.init_ok = false;
    try { app_main(); } catch (const std::runtime_error &) {}
    CHECK(ui.screen == "idle" && !audio_ready && ui.sd_status && sd_mounted);
    CHECK(ui.renders_without_status == 0);
    reset(); button.init_ok = false; app_main();
    CHECK(ui.screen == "error" && !ui.recoverable && ui.renders_without_status == 0);
    reset();
    try { app_main(); } catch (const std::runtime_error &) {}
    CHECK(ui.screen == "idle" && ui.wifi_status && ui.sd_status && sd_mounted);
    CHECK(ui.renders_without_status == 0);
    handleButton(Event::LongPress); // Settings.
    handleButton(Event::LongPress); // Empty history.
    handleButton(Event::LongPress); // Return.
    handleButton(Event::ShortPress); handleButton(Event::LongPress); // Wi-Fi info.
    CHECK(ui.renders_without_status == 0);
    handleButton(Event::LongPress); menu_index = 2; store.pending = {"p"}; executeMenuItem();
    CHECK(ui.renders_without_status == 0 && ui.wifi_status && ui.sd_status);
    wifi.online = false; handleButton(Event::ShortPress);
    CHECK(!ui.wifi_status && ui.renders_without_status == 0);
    reset(); sd_mounted = true; renderIdle();
    handleButton(Event::ShortPress);
    wifi.online = true; mock_now_us += 3000000; pollRecording();
    CHECK(ui.renders_without_status == 0 && ui.wifi_status && ui.sd_status);
    handleButton(Event::LongPress); recorder.clean = false; pollRecording();
    CHECK(ui.screen == "error" && ui.recoverable && ui.renders_without_status == 0);
    return true;
}
bool menu_guards() {
    for (auto view : {MenuView::Settings, MenuView::Notes, MenuView::Reader, MenuView::Info}) {
        reset(); state = AppState::Menu; menu_view = view;
        wifi.online = true; store.pending = {"p"};
        for (auto command : {network::web::Command::Start, network::web::Command::Stop, network::web::Command::Sync}) {
            web.command = command; web.queued = true; handleWebCommand();
            CHECK(web.result == "rejected" && state == AppState::Menu);
            CHECK(recorder.starts == 0 && api.calls == 0 && recorder.stops == 0);
        }
        reset(); int iterations = 0;
        queue_hook = [&]() {
            if (iterations++ != 0) throw std::runtime_error("end after one main-loop iteration");
            state = AppState::Menu; menu_view = view;
            wifi.online = false; store.pending = {"p"};
            last_wifi_attempt_us = last_sync_attempt_us = -100000000;
            return 0;
        };
        try { app_main(); } catch (const std::runtime_error &) {}
        CHECK(iterations == 2 && state == AppState::Menu && wifi.connects == 1 && api.calls == 0);
        queue_hook = {};
        // The connected path must not auto-sync either.
        reset(); iterations = 0;
        queue_hook = [&]() {
            if (iterations++ != 0) throw std::runtime_error("end after one main-loop iteration");
            state = AppState::Menu; menu_view = view;
            store.pending = {"p"}; last_sync_attempt_us = -100000000;
            return 0;
        };
        try { app_main(); } catch (const std::runtime_error &) {}
        CHECK(state == AppState::Menu && api.calls == 0);
        queue_hook = {};
    }
    return true;
}
bool worker_safety() {
    reset(); handleButton(Event::ShortPress); handleButton(Event::DoublePress);
    recorder.stopped = false; preview.stopped = false;
    pollRecording();
    CHECK(state == AppState::Recording && store.commits == 0 && stopping_recording);
    recorder.stopped = true; pollRecording();
    CHECK(state == AppState::Recording && store.commits == 0 && stopping_recording);
    preview.stopped = true; pollRecording();
    CHECK(state == AppState::Idle && store.commits == 1 && !stopping_recording);
    CHECK(last_note_id == "recorded" && ui.screen == "idle");
    reset(); recorder.start_ok = false; preview.stopped = false;
    handleButton(Event::ShortPress); pollRecording();
    CHECK(state == AppState::Recording && recording_start_failed);
    preview.stopped = true; pollRecording();
    CHECK(state == AppState::Idle && store.commits == 0);
    for (bool clean : {false, true}) {
        reset(); recorder.clean = clean; recorder.bytes = 10;
        handleButton(Event::DoublePress); handleButton(Event::LongPress); pollRecording();
        CHECK(store.commits == 0 && store.discards == (clean ? 1 : 0));
        CHECK(recording_recovery_required == !clean);
        CHECK(ui.renders_without_status == 0);
    }
    reset(); store.commit_ok = false;
    handleButton(Event::DoublePress); handleButton(Event::ShortPress); pollRecording();
    CHECK(recording_recovery_required && store.discards == 0);
    handleButton(Event::ShortPress); CHECK(recorder.starts == 1 && state == AppState::Idle);
    handleButton(Event::LongPress); CHECK(state == AppState::Menu);
    CHECK(ui.renders_without_status == 0);
    return true;
}
bool idle_sync_and_web() {
    reset(); wifi.online = true; store.pending = {"p"};
    CHECK(syncPending() && state == AppState::Idle && store.archives == 1);
    CHECK(web.snapshots.front().state == AppState::Syncing);
    CHECK(web.snapshots.back().state == AppState::Idle && ui.renders_without_status == 0);
    reset(); web.queued = true; web.command = network::web::Command::Start; handleWebCommand();
    CHECK(state == AppState::Recording && web.result == "ok");
    web.queued = true; web.command = network::web::Command::Stop; handleWebCommand();
    CHECK(web.result == "stopping" && stopping_recording);
    return true;
}
bool empty_transcript() {
    reset(); store.history = {{"empty", true}}; store.transcript.clear();
    display::Ui::pages = 1; open_row(0); handleButton(Event::LongPress);
    CHECK(ui.screen == "reader" && ui.message.empty());
    handleButton(Event::DoublePress); CHECK(ui.page == 0);
    display::Ui::pages = 0; handleButton(Event::ShortPress); CHECK(ui.page == 0);
    handleButton(Event::LongPress); CHECK(reader_text.empty());
    CHECK(ui.renders_without_status == 0);
    return true;
}
int main() {
    int passed = 0;
    if (!gestures()) return 1;
    ++passed;
    if (!history_reader()) return 1;
    ++passed;
    if (!settings_actions()) return 1;
    ++passed;
    if (!status_and_startup()) return 1;
    ++passed;
    if (!menu_guards()) return 1;
    ++passed;
    if (!worker_safety()) return 1;
    ++passed;
    if (!idle_sync_and_web()) return 1;
    ++passed;
    if (!empty_transcript()) return 1;
    ++passed;
    std::cout << "PASS " << passed << " navigation scenarios (real main.cpp, mocked hardware)\n";
}
'''

def main():
    source = (ROOT / "firmware/src/main.cpp").read_text()
    source = re.sub(r'^#include[^\n]*$', '', source, flags=re.MULTILINE)
    scratch = Path(os.environ.get("TMPDIR", Path.home() / ".hermes/cache/scratch"))
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="navigation-", dir=scratch) as temp:
        path = Path(temp)
        translation = path / "navigation.cpp"
        binary = path / "navigation_tests"
        translation.write_text('#include "navigation_mocks.h"\n' + source + '\n' + TESTS)
        command = shlex.split(os.environ.get("CXX", "g++")) + [
            "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-variable",
            *shlex.split(os.environ.get("HOST_TEST_FLAGS", "")),
            "-I", str(ROOT / "firmware/tests/stubs"),
            "-I", str(ROOT / "firmware/src"),
            "-I", str(ROOT / "firmware/include"),
            str(translation), "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
