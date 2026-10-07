#include <string>
#include <vector>

#include "app/button.h"
#include "audio/audio_recorder.h"
#include "display/epaper_display.h"
#include "display/ui.h"
#include "network/api_client.h"
#include "network/server_url.h"
#include "network/live_preview.h"
#include "network/live_preview_helpers.h"
#include "network/wifi_manager.h"
#include "network/web_server.h"
#include "project_config.h"
#include "storage/note_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace {

const char *TAG = "voice_notes";

using AppState = network::web::State;

display::EpaperDisplay epaper;
display::Ui ui(epaper);
storage::NoteStore store;
audio::AudioRecorder recorder;
network::WifiManager wifi;
network::WebServer web;
network::ApiClient api;
network::LivePreview preview;
network::live::RefreshGate live_refresh;
app::Button button;

AppState state = AppState::Idle;
std::string active_note_id;
std::string last_note_id;
std::string last_note_text;
std::string live_text;
bool stopping_recording = false;
bool recording_start_failed = false;
bool recording_recovery_required = false;
bool sd_mounted = false;
bool audio_ready = false;
size_t menu_index = 0;
// Main-task-owned subviews keep the web policy and idle-only auto-sync gate intact.
enum class MenuView { Settings, Notes, Reader, Info, Refresh };
constexpr uint8_t REFRESH_OPTIONS[] = {1, 5, 10, 20, 50, 100, 0};
uint8_t refresh_limit = 10;
size_t refresh_option = 2;

void loadServerUrl() {
    network::ApiClient::setBaseUrl(network::ApiClient::defaultBaseUrl());
    nvs_handle_t handle;
    if (nvs_open("server", NVS_READONLY, &handle) != ESP_OK) return;
    char url[64]{}; size_t size = sizeof(url);
    if (nvs_get_str(handle, "base_url", url, &size) == ESP_OK &&
        network::validServerUrl(url, wifi.ipAddress())) network::ApiClient::setBaseUrl(url);
    nvs_close(handle);
}

bool saveServerUrl(const char *url) {
    nvs_handle_t handle;
    if (nvs_open("server", NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_str(handle, "base_url", url) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (ok) network::ApiClient::setBaseUrl(url);
    return ok;
}

void loadRefreshInterval() {
    refresh_limit = 10;
    nvs_handle_t handle;
    if (nvs_open("display", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t saved = 10;
        if (nvs_get_u8(handle, "partial_limit", &saved) == ESP_OK) {
            for (auto option : REFRESH_OPTIONS) if (option == saved) refresh_limit = saved;
        }
        nvs_close(handle);
    }
    epaper.setPartialRefreshLimit(refresh_limit);
}

bool saveRefreshInterval(uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("display", NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_u8(handle, "partial_limit", value) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (ok) {
        refresh_limit = value;
        epaper.setPartialRefreshLimit(value);
    }
    return ok;
}
MenuView menu_view = MenuView::Settings;
constexpr size_t SAVED_NOTES_LIMIT = 100;
std::vector<display::SavedNote> saved_notes;
size_t note_index = 0;
size_t reader_page = 0;
std::string reader_text;
std::string info_title;
std::string info_message;
int64_t last_wifi_attempt_us = 0;
int64_t last_sync_attempt_us = 0;

void publishWeb(bool blocked = false) {
    static network::WebStatus snapshot;
    // DHCP can reveal a saved address is now this device. Recheck only when workers are idle.
    if ((state == AppState::Idle || state == AppState::Menu) && !stopping_recording &&
        network::ApiClient::baseUrl() != network::ApiClient::defaultBaseUrl() &&
        !network::validServerUrl(network::ApiClient::baseUrl(), wifi.ipAddress()))
        network::ApiClient::setBaseUrl(network::ApiClient::defaultBaseUrl());
    snapshot.state = state;
    network::web::copyText(snapshot.base_url, network::ApiClient::baseUrl());
    snapshot.connecting = blocked;
    snapshot.wifi = wifi.connected();
    snapshot.stopping = stopping_recording;
    snapshot.recovery = recording_recovery_required;
    // Avoid repeated SD directory scans during real-time WAV capture.
    if (state != AppState::Recording) snapshot.pending = store.pendingCount();
    network::web::copyText(snapshot.ip, wifi.ipAddress());
    network::web::copyText(snapshot.note_id, state == AppState::Recording ? active_note_id : last_note_id);
    network::web::copyText(snapshot.text, last_note_text);
    network::web::copyText(snapshot.live_text, live_text);
    web.publish(snapshot);
}

void renderIdle() {
    ui.setStatus(wifi.connected(), sd_mounted);
    const size_t pending = store.pendingCount();
    if (!wifi.connected() && pending > 0) {
        ui.showOffline(pending, last_note_id, last_note_text);
    } else {
        ui.showIdle(last_note_id, last_note_text, pending, wifi.connected());
    }
}

bool syncPending() {
    const bool from_menu = state == AppState::Menu;
    if (!wifi.connected()) {
        if (!from_menu) renderIdle();
        return false;
    }

    auto pending = store.pendingIds();
    if (pending.empty()) {
        if (!from_menu) renderIdle();
        return true;
    }

    state = AppState::Syncing;
    publishWeb();
    bool all_ok = true;

    for (size_t i = 0; i < pending.size(); ++i) {
        const std::string &id = pending[i];
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showSyncing(i + 1, pending.size(), id);
        const auto result = api.transcribe(id, store.pendingAudioPath(id));
        if (!result.ok) {
            ESP_LOGW(TAG, "Sync failed for %s: %s", id.c_str(), result.error.c_str());
            all_ok = false;
            break;
        }

        if (!store.writeTranscript(id, result.text, result.language, result.duration, result.model)) {
            ESP_LOGE(TAG, "Cannot persist transcript for %s", id.c_str());
            all_ok = false;
            break;
        }

        if (!store.archiveAudio(id)) {
            ESP_LOGW(TAG, "Transcript saved but WAV could not be archived: %s", id.c_str());
        }

        last_note_id = id;
        last_note_text = result.text;
        publishWeb();
    }

    state = from_menu ? AppState::Menu : AppState::Idle;
    publishWeb();
    if (!from_menu) renderIdle();
    return all_ok;
}

void startRecording() {
    if (state == AppState::Recording) {
        return;
    }
    // Optional hardware failures must not disable button/menu navigation.
    if (!sd_mounted || !audio_ready) {
        ui.setStatus(wifi.connected(), sd_mounted);
        if (!sd_mounted) ui.showStorageError();
        else ui.showError("AUDIO", "Audio unavailable. Hold to open Settings.");
        return;
    }
    if (recording_recovery_required) {
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("SD", "WAV retained: recover it before a new note");
        return;
    }
    active_note_id = store.makeNoteId();
    live_text.clear();
    stopping_recording = false;
    recording_start_failed = false;
    state = AppState::Recording;
    publishWeb();
    const bool live_started = preview.start(active_note_id);
    if (!live_started) ESP_LOGW(TAG, "Live preview disabled; local WAV recording continues");
    if (!recorder.start(store.recordingTempPath(), live_started ? &preview : nullptr)) {
        preview.requestStop();
        stopping_recording = true;
        recording_start_failed = true;
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("AUDIO", "Cannot start recording");
        state = AppState::Recording; // Poll worker completion before another start.
        return;
    }
    state = AppState::Recording;
    ui.setStatus(wifi.connected(), sd_mounted);
    ui.showLiveRecording(active_note_id, live_text, wifi.connected());
    live_refresh.reset(esp_timer_get_time() / 1000, live_text, wifi.connected());
}

void stopRecording() {
    if (state != AppState::Recording) {
        return;
    }
    recorder.requestStop();
    preview.requestStop();
    stopping_recording = true;
}

void pollRecording() {
    if (!stopping_recording && !recorder.isRecording()) stopRecording();
    if (!stopping_recording) {
        preview.snapshot(live_text);
        const bool connected = wifi.connected();
        if (live_refresh.changed(esp_timer_get_time() / 1000, live_text, connected)) {
            ui.setStatus(connected, sd_mounted);
            ui.showLiveRecording(active_note_id, live_text, connected);
            // Throttle from completed refresh, not its start (full e-paper refresh is slow).
            live_refresh.reset(esp_timer_get_time() / 1000, live_text, connected);
        }
        return;
    }
    // Never block main on network/SD teardown and never free a producer's queue early.
    if (!recorder.waitStopped(0) || !preview.waitStopped(0)) return;
    stopping_recording = false;
    live_text.clear();
    if (recording_start_failed) {
        active_note_id.clear();
        state = AppState::Idle;
        return;
    }

    if (!recorder.savedCleanly()) {
        recording_recovery_required = true;
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("SD", "Audio/SD error: WAV retained for recovery");
        state = AppState::Idle;
        return;
    }

    if (network::live::canDiscardShortWav(recorder.recordedBytes(), recorder.savedCleanly())) {
        store.discardRecording();
        active_note_id.clear();
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("NOTE TOO SHORT", "No usable audio recorded");
        state = AppState::Idle;
        return;
    }

    if (!store.commitRecording(active_note_id)) {
        recording_recovery_required = true; // Do not overwrite the retained temporary WAV.
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("SD", "Could not finalize the note");
        state = AppState::Idle;
        return;
    }

    last_note_id = active_note_id;
    last_note_text = "Audio saved. Transcription pending.";
    active_note_id.clear();
    state = AppState::Idle;

    if (wifi.connected()) {
        syncPending();
    } else {
        renderIdle();
    }
}

void handleWebCommand() {
    network::web::Command command{};
    unsigned id = 0;
    if (!web.take(command, id)) return;
    // Revalidate on main: a physical button may have changed state after enqueue.
    if ((command == network::web::Command::Start && (!sd_mounted || !audio_ready)) ||
        (command == network::web::Command::Sync && !sd_mounted) ||
        !network::web::allowed(command, state, stopping_recording,
            recording_recovery_required, wifi.connected(), store.pendingCount())) {
        publishWeb();
        web.complete(id, "rejected");
        return;
    }
    const char *result = "ok";
    switch (command) {
        case network::web::Command::Configure: {
            char url[64]{}; web.takeConfig(url);
            // Recording state persists until both workers have joined; sync runs on main.
            if (!network::validServerUrl(url, wifi.ipAddress())) result = "rejected";
            else if (!saveServerUrl(url)) result = "failed";
            break;
        }
        case network::web::Command::Start:
            startRecording();
            if (recording_start_failed || !recorder.isRecording()) result = "failed";
            break;
        case network::web::Command::Stop:
            stopRecording();
            result = "stopping"; // Not a claim that the WAV has been finalized.
            break;
        case network::web::Command::Sync:
            if (!syncPending()) result = "failed";
            break;
    }
    publishWeb();
    web.complete(id, result);
}

void renderMenu() {
    ui.setStatus(wifi.connected(), sd_mounted);
    switch (menu_view) {
        case MenuView::Settings:
            ui.showMenu(menu_index, refresh_limit);
            break;
        case MenuView::Refresh:
            ui.showRefreshInterval(REFRESH_OPTIONS[refresh_option]);
            break;
        case MenuView::Notes:
            ui.showSavedNotes(saved_notes, note_index);
            break;
        case MenuView::Reader:
            ui.showNote(saved_notes[note_index].id, reader_text, reader_page);
            break;
        case MenuView::Info:
            ui.showInfo(info_title, info_message);
            break;
    }
}

void showMenuInfo(const std::string &title, const std::string &message) {
    info_title = title;
    info_message = message;
    menu_view = MenuView::Info;
    renderMenu();
}

void reconnectWifi() {
    last_wifi_attempt_us = esp_timer_get_time();
    publishWeb(true);
    wifi.connectPreferred();
    publishWeb();
}

void executeMenuItem() {
    if (menu_view == MenuView::Refresh) {
        if (saveRefreshInterval(REFRESH_OPTIONS[refresh_option])) {
            menu_view = MenuView::Settings;
            renderMenu();
        } else {
            showMenuInfo("Save failed", "Refresh interval not saved. Previous setting remains active. Return to retry.");
        }
        return;
    }
    if (menu_view == MenuView::Reader) {
        std::string().swap(reader_text);
        menu_view = MenuView::Notes;
        renderMenu();
        return;
    }
    if (menu_view == MenuView::Info) {
        menu_view = MenuView::Settings;
        renderMenu();
        return;
    }
    if (menu_view == MenuView::Notes) {
        if (note_index == saved_notes.size()) {
            menu_view = MenuView::Settings;
        } else {
            const auto &note = saved_notes[note_index];
            reader_text.clear();
            if (!note.transcribed) {
                reader_text = "Audio saved. Transcription unavailable.";
            } else if (!store.readTranscript(note.id, reader_text)) {
                reader_text = "Cannot read the transcript from the SD card.";
            }
            reader_page = 0;
            menu_view = MenuView::Reader;
        }
        renderMenu();
        return;
    }

    switch (menu_index) {
        case 0:
            saved_notes.clear();
            for (const auto &note : store.savedNotes(SAVED_NOTES_LIMIT)) {
                saved_notes.push_back({note.id, note.transcribed});
            }
            note_index = 0;
            menu_view = MenuView::Notes;
            renderMenu();
            break;
        case 1:
            reconnectWifi();
            showMenuInfo("Wi-Fi", wifi.connected()
                ? "SSID: " + wifi.currentSsid() + "\nIP: " + wifi.ipAddress()
                : "Wi-Fi unavailable. No network connected.");
            break;
        case 2: {
            if (!wifi.connected()) reconnectWifi();
            if (!wifi.connected()) {
                showMenuInfo("Synchronization", "No network. Notes retained on SD.");
            } else if (store.pendingCount() == 0) {
                showMenuInfo("Synchronization", "No pending notes.");
            } else {
                last_sync_attempt_us = esp_timer_get_time();
                const bool ok = syncPending();
                const size_t remaining = store.pendingCount();
                showMenuInfo("Synchronization", !ok
                    ? "Failed. Pending notes: " + std::to_string(remaining)
                    : remaining > 0
                        ? "Transcript saved. Pending WAVs: " + std::to_string(remaining)
                        : "Synchronization complete.");
            }
            break;
        }
        case 3:
            // Explicit retry only while not mounted; never format or replace a live mount.
            if (!sd_mounted) sd_mounted = store.init();
            showMenuInfo("Storage", std::string(sd_mounted ? "SD mounted." : "SD unavailable. Insert card, return and select Storage to retry.") +
                "\nPending notes: " + std::to_string(store.pendingCount()) +
                (audio_ready ? "" : "\nAudio unavailable. Recording disabled."));
            break;
        case 4:
            showMenuInfo("About", "Waveshare ESP32-S3\nePaper 1.54 V2\n200x200 black/white\nVoice Notes / Whistle");
            break;
        case 6:
            for (size_t i = 0; i < sizeof(REFRESH_OPTIONS); ++i)
                if (REFRESH_OPTIONS[i] == refresh_limit) refresh_option = i;
            menu_view = MenuView::Refresh;
            renderMenu();
            break;
        case 5:
            std::vector<display::SavedNote>().swap(saved_notes);
            std::string().swap(reader_text);
            std::string().swap(info_title);
            std::string().swap(info_message);
            note_index = reader_page = menu_index = 0;
            menu_view = MenuView::Settings;
            state = AppState::Idle;
            renderIdle();
            break;
        default:
            break;
    }
}

void handleButton(app::ButtonEvent event) {
    switch (state) {
        case AppState::Recording:
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress ||
                event == app::ButtonEvent::LongPress) {
                stopRecording();
            }
            return;

        case AppState::Menu:
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress) {
                if (menu_view == MenuView::Settings) {
                    menu_index = (menu_index + 1) % display::Ui::MENU_ITEMS;
                } else if (menu_view == MenuView::Refresh) {
                    refresh_option = (refresh_option + 1) % sizeof(REFRESH_OPTIONS);
                } else if (menu_view == MenuView::Notes) {
                    note_index = (note_index + 1) % (saved_notes.size() + 1);
                } else if (menu_view == MenuView::Reader) {
                    const size_t pages = display::Ui::notePageCount(reader_text);
                    reader_page = pages ? (reader_page + 1) % pages : 0;
                }
                renderMenu();
            } else if (event == app::ButtonEvent::LongPress) {
                executeMenuItem();
            }
            return;

        case AppState::Syncing:
            // Synchronization is deliberately non-interruptible in the MVP.
            return;

        case AppState::Idle:
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress) {
                startRecording();
            } else if (event == app::ButtonEvent::LongPress) {
                state = AppState::Menu;
                menu_index = 0;
                menu_view = MenuView::Settings;
                renderMenu();
            }
            return;
    }
}

} // namespace

extern "C" void app_main(void) {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    loadServerUrl();
    loadRefreshInterval();

    if (!epaper.init()) {
        ESP_LOGE(TAG, "ePaper init failed");
        return;
    }
    ui.setStatus(wifi.connected(), sd_mounted);
    ui.showBoot();

    sd_mounted = store.init();
    if (!sd_mounted) {
        ESP_LOGW(TAG, "SD unavailable; Settings remains usable (Storage retries mounting)");
    }

    audio_ready = recorder.init();
    if (!audio_ready) {
        ESP_LOGW(TAG, "Audio unavailable; recording disabled, Settings remains usable");
    }

    if (!button.init()) {
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showError("BUTTON ERROR", "Button GPIO unavailable", false);
        return;
    }

    display::BootWifiStatus home = display::BootWifiStatus::Pending;
    display::BootWifiStatus hotspot = display::BootWifiStatus::Pending;
    std::string boot_ip;
    ui.setStatus(false, sd_mounted);
    ui.showBootWifi(home, hotspot);
    wifi.connectPreferred([&](size_t index, network::WifiAttemptStatus status, const std::string &ip) {
        display::BootWifiStatus mapped = display::BootWifiStatus::Failed;
        switch (status) {
            case network::WifiAttemptStatus::Connecting: mapped = display::BootWifiStatus::Connecting; break;
            case network::WifiAttemptStatus::Connected: mapped = display::BootWifiStatus::Connected; boot_ip = ip; break;
            case network::WifiAttemptStatus::Failed: mapped = display::BootWifiStatus::Failed; break;
            case network::WifiAttemptStatus::Skipped: mapped = display::BootWifiStatus::Skipped; break;
            case network::WifiAttemptStatus::Disabled: mapped = display::BootWifiStatus::Disabled; break;
        }
        (index == 0 ? home : hotspot) = mapped;
        ui.setStatus(home == display::BootWifiStatus::Connected || hotspot == display::BootWifiStatus::Connected, sd_mounted);
        ui.showBootWifi(home, hotspot, boot_ip);
    });
    last_wifi_attempt_us = esp_timer_get_time();
    renderIdle();
    if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
    publishWeb();

    if (wifi.connected() && store.pendingCount() > 0) {
        syncPending();
    }

    while (true) {
        handleWebCommand();
        app::ButtonEvent event{};
        if (xQueueReceive(button.queue(), &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            handleButton(event);
        }

        if (state == AppState::Recording) {
            pollRecording();
        }

        publishWeb();
        if (state != AppState::Idle) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (!wifi.connected() &&
            (now - last_wifi_attempt_us) / 1000 >= config::WIFI_RETRY_IDLE_MS) {
            last_wifi_attempt_us = now;
            publishWeb(true);
            if (wifi.connectPreferred()) {
                renderIdle();
            }
        }

        if (wifi.connected() && store.pendingCount() > 0 &&
            (now - last_sync_attempt_us) / 1000 >= config::SYNC_RETRY_MS) {
            last_sync_attempt_us = now;
            syncPending();
        }
        publishWeb();
    }
}
