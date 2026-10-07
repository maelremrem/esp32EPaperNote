#include <string>
#include <algorithm>
#include <iterator>
#include <vector>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app/button.h"
#include "audio/audio_recorder.h"
#include "display/epaper_display.h"
#include "display/ui.h"
#include "network/api_client.h"
#include "network/server_url.h"
#include "network/live_preview.h"
#include "network/live_preview_helpers.h"
#include "network/wifi_manager.h"
#include "network/captive_portal.h"
#include "network/web_server.h"
#include "project_config.h"
#include "storage/note_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
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
network::CaptivePortal portal;
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
enum class MenuView { Settings, Notes, Reader, Info, Refresh, Wifi, Storage, Format, Portal, Synchronization, Pending, CancelPending };
MenuView info_parent = MenuView::Settings;
size_t submenu_index = 0;
bool erase_selected = false;
bool cancel_selected = false, cancel_all = false;
std::string cancel_id;
std::vector<std::string> pending_notes;
size_t pending_index = 0;
constexpr uint8_t REFRESH_OPTIONS[] = {1, 5, 10, 20, 50, 100, 0};
uint8_t refresh_limit = 10;
size_t refresh_option = 2;

unsigned server_revision=0;
void loadServerUrl() {
    network::ApiClient::setBaseUrl(network::ApiClient::defaultBaseUrl());
    nvs_handle_t handle;
    if (nvs_open("server", NVS_READONLY, &handle) != ESP_OK) return;
    char url[64]{}; size_t size = sizeof(url);
    if (nvs_get_str(handle, "base_url", url, &size) == ESP_OK &&
        network::validServerUrl(url, wifi.ipAddress())) network::ApiClient::setBaseUrl(url);
    char token[193]{}; size=sizeof(token);
    if (nvs_get_str(handle,"token",token,&size)==ESP_OK && network::web::validServerToken(token))
        network::ApiClient::setTarget(network::ApiClient::baseUrl(),token);
    std::fill(std::begin(token),std::end(token),0);
    nvs_close(handle);
}

bool saveServerUrl(const char *url, const network::web::SettingsWrite &settings = {}) {
    nvs_handle_t handle;
    if (nvs_open("server", NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_str(handle, "base_url", url) == ESP_OK;
    if (ok && settings.replace_token) ok=nvs_set_str(handle,"token",settings.server_token)==ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (ok) { network::ApiClient::setTarget(url,settings.server_token,settings.replace_token); ++server_revision; }
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

unsigned format_challenge=0;
int64_t format_deadline=0;
void publishWeb(bool blocked = false) {
    static network::WebStatus snapshot;
    // DHCP can reveal a saved address is now this device. Recheck only when workers are idle.
    if ((state == AppState::Idle || state == AppState::Menu) && !stopping_recording &&
        network::ApiClient::baseUrl() != network::ApiClient::defaultBaseUrl() &&
        !network::validServerUrl(network::ApiClient::baseUrl(), wifi.ipAddress()))
        network::ApiClient::setBaseUrl(network::ApiClient::defaultBaseUrl());
    if (state!=AppState::Idle && state!=AppState::Menu) format_challenge=0;
    if (portal.active() || menu_view==MenuView::Format || esp_timer_get_time()>=format_deadline) format_challenge=0;
    snapshot.token_configured=network::ApiClient::tokenConfigured(); snapshot.server_revision=server_revision;
    snapshot.mounted=sd_mounted;
    const auto usage=!web.downloadBusy() && sd_mounted && !stopping_recording && (state==AppState::Idle || state==AppState::Menu) &&
        recorder.waitStopped(0) && preview.waitStopped(0) ? store.usage() : storage::Usage{};
    snapshot.usage_known=usage.known; snapshot.total_bytes=usage.total; snapshot.free_bytes=usage.free;
    network::web::copyText(snapshot.storage_error, store.lastError());
    snapshot.audio_ready=audio_ready;
    snapshot.partial_limit=refresh_limit;
    snapshot.format_challenge=format_challenge;
    const auto ssids=wifi.savedSsids();
    for (size_t i=0;i<2;++i) network::web::copyText(snapshot.ssid[i], ssids[i]);
    network::web::copyText(snapshot.current_ssid, wifi.currentSsid());
    snapshot.state = state;
    network::web::copyText(snapshot.base_url, network::ApiClient::baseUrl());
    snapshot.connecting = blocked || portal.active() || menu_view == MenuView::Format || menu_view == MenuView::CancelPending;
    snapshot.wifi = wifi.connected();
    snapshot.stopping = stopping_recording;
    snapshot.recovery = recording_recovery_required;
    // Avoid repeated SD directory scans during real-time WAV capture.
    if (!web.downloadBusy() && (state==AppState::Idle || state==AppState::Menu) && recorder.waitStopped(0) && preview.waitStopped(0)) {
        snapshot.pending=store.pendingCount();
        snapshot.notes_count=0;
        for(const auto &note:store.savedNotes(40)) {
            auto &out=snapshot.notes[snapshot.notes_count++]; network::web::copyText(out.id,note.id);
            out.transcribed=note.transcribed; out.audio=note.audio;
        }
    }
    network::web::copyText(snapshot.ip, wifi.ipAddress());
    network::web::copyText(snapshot.note_id, state == AppState::Recording ? active_note_id : last_note_id);
    network::web::copyText(snapshot.text, last_note_text);
    network::web::copyText(snapshot.live_text, live_text);
    web.publish(snapshot);
}

void renderIdle() {
    if(web.downloadBusy()) return;
    ui.setStatus(wifi.connected(), sd_mounted);
    const size_t pending = store.pendingCount();
    if (!wifi.connected() && pending > 0) {
        ui.showOffline(pending, last_note_id, last_note_text);
    } else {
        ui.showIdle(last_note_id, last_note_text, pending, wifi.connected());
    }
}

bool sync_cancelled = false;
bool auto_sync_paused = false;
std::atomic<bool> sync_stop{false};
void handleButton(app::ButtonEvent event);
struct SyncRequest {
    std::string id, path;
    network::TranscriptResult result;
    SemaphoreHandle_t done = nullptr;
};
void syncTask(void *argument) {
    auto *request = static_cast<SyncRequest *>(argument);
    request->result = api.transcribe(request->id, request->path, &sync_stop);
    // Completion joins all accesses to request, its WAV and the HTTP handle.
    xSemaphoreGive(request->done);
    vTaskDelete(nullptr);
}
void pollSyncButtons() {
    app::ButtonEvent event{};
    while (xQueueReceive(button.queue(), &event, 0) == pdTRUE) handleButton(event);
}
bool syncPending() {
    sync_cancelled = false;
    if(state == AppState::Syncing || web.downloadBusy() || !sd_mounted || stopping_recording || !recorder.waitStopped(0) || !preview.waitStopped(0)) return false;
    sync_stop.store(false);
    auto_sync_paused = false; // An explicit new batch resumes automatic retry.
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
        pollSyncButtons();
        if (sync_stop.load()) { sync_cancelled = auto_sync_paused = true; all_ok = false; break; }
        const std::string &id = pending[i];
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showSyncing(i + 1, pending.size(), id);
        SyncRequest request;
        request.id = id;
        request.path = store.pendingAudioPath(id); // Main owns NoteStore.
        request.done = xSemaphoreCreateBinary();
        if (!request.done) { all_ok = false; break; }
        if (xTaskCreate(syncTask, "note_sync", 12288, &request, 3, nullptr) != pdPASS) {
            vSemaphoreDelete(request.done); all_ok = false; break;
        }
        bool joined = false;
        do {
            pollSyncButtons();
            publishWeb(); // Remain Syncing; storage/config commands stay blocked.
            joined = xSemaphoreTake(request.done, pdMS_TO_TICKS(50)) == pdTRUE;
        } while (!joined);
        pollSyncButtons(); // A stop queued alongside completion wins before commit.
        vSemaphoreDelete(request.done);
        const auto &result = request.result;
        if (sync_stop.load() || result.cancelled) {
            sync_cancelled = auto_sync_paused = true;
            all_ok = false;
            break;
        }
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
    if (sync_cancelled && !from_menu) {
        state = AppState::Menu;
        info_parent = MenuView::Settings;
        menu_view = MenuView::Info;
        info_title = "Sync cancelled";
        info_message = "Pending audio kept.\nServer work may finish.\nUse Sync now to retry.";
    }
    publishWeb();
    if (sync_cancelled) {
        ui.setStatus(wifi.connected(), sd_mounted);
        ui.showInfo("Sync cancelled", "Pending audio kept.\nServer work may finish.\nUse Sync now to retry.");
    } else if (!from_menu) renderIdle();
    return all_ok;
}

void startRecording() {
    if (web.downloadBusy() || portal.active() || menu_view == MenuView::Format) return;
    if (state == AppState::Recording || state == AppState::Syncing) {
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

    if (wifi.connected() && !auto_sync_paused) {
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
    if ((web.downloadBusy() && command!=network::web::Command::Download) || portal.active() || menu_view == MenuView::Format || menu_view == MenuView::CancelPending ||
        (command != network::web::Command::Start && command != network::web::Command::Stop &&
         command != network::web::Command::Sync && (!recorder.waitStopped(0) || !preview.waitStopped(0))) ||
        (command == network::web::Command::Start && (!sd_mounted || !audio_ready)) ||
        (command == network::web::Command::Sync && !sd_mounted) ||
        !network::web::allowed(command, state, stopping_recording,
            recording_recovery_required, wifi.connected(), store.pendingCount())) {
        format_challenge=0;
        publishWeb();
        web.complete(id, "rejected");
        return;
    }
    const char *result = "ok";
    network::web::SettingsWrite settings;
    web.takeSettings(settings);
    const unsigned challenge=format_challenge;
    const bool challenge_valid=challenge && settings.challenge==challenge && esp_timer_get_time()<format_deadline;
    format_challenge=0; // Every command invalidates prior confirmation, including failed writes.
    switch (command) {
        case network::web::Command::Download: {
            char note[97]{}; bool markdown=false; uint64_t bytes=0;
            FILE *file=nullptr;
            if(sd_mounted && recorder.waitStopped(0) && preview.waitStopped(0) && web.takeDownload(note,markdown))
                file=store.openDownload(note,markdown,bytes);
            web.provideDownload(id,file,bytes);
            return; // Handler retains mailbox/SD lease until FILE is closed, including send abort.
        }
        case network::web::Command::Display:
            if (std::find(std::begin(REFRESH_OPTIONS),std::end(REFRESH_OPTIONS),settings.partial_limit)==std::end(REFRESH_OPTIONS)) result="rejected";
            else if (!recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else if (!saveRefreshInterval(settings.partial_limit)) result="failed";
            break;
        case network::web::Command::Wifi: {
            network::WifiProfiles edits;
            for (size_t i=0;i<2;++i) { edits[i].ssid=settings.ssid[i]; edits[i].password=settings.password[i]; }
            if (!recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else if (!wifi.saveProfileEdits(edits, {{settings.open[0],settings.open[1]}})) result="failed";
            break;
        }
        case network::web::Command::Reconnect:
            if (!recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else { publishWeb(true); if (!wifi.connectPreferred()) result="failed"; }
            break;
        case network::web::Command::Mount:
            if (!recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else if (!sd_mounted) { sd_mounted=store.init(); if (!sd_mounted) result="failed"; }
            break;
        case network::web::Command::PrepareFormat:
            if (!sd_mounted || !recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else { format_challenge=esp_random(); if (!format_challenge) format_challenge=1; format_deadline=esp_timer_get_time()+60000000; }
            break;
        case network::web::Command::Format:
            if (!challenge_valid || !sd_mounted || !recorder.waitStopped(0) || !preview.waitStopped(0)) result="rejected";
            else {
                saved_notes.clear(); reader_text.clear(); active_note_id.clear();
                last_note_id.clear(); last_note_text.clear(); live_text.clear(); note_index=reader_page=0;
                // Exit a physical reader that would otherwise index the erased cache.
                menu_view=MenuView::Settings;
                sd_mounted=store.format(); recording_recovery_required=!sd_mounted;
                if (!sd_mounted) result="failed";
                if (state==AppState::Menu) { ui.setStatus(wifi.connected(),sd_mounted); ui.showMenu(menu_index,refresh_limit); }
                else renderIdle();
            }
            break;
        case network::web::Command::Configure: {
            char url[64]{}; web.takeConfig(url);
            // Recording state persists until both workers have joined; sync runs on main.
            if (!network::validServerUrl(url, wifi.ipAddress()) || !network::web::validServerToken(settings.server_token)) result = "rejected";
            else if (!saveServerUrl(url,settings)) result = "failed";
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
            if (!syncPending()) result = sync_cancelled ? "cancelled" : "failed";
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
        case MenuView::Synchronization:
            ui.showSubmenu("Synchronization", {"Sync now", "Pending notes", "Cancel all pending", "Back"}, submenu_index,
                           {false, true, true, false});
            break;
        case MenuView::Pending: {
            auto entries = pending_notes;
            entries.push_back("Back");
            std::vector<bool> children(entries.size(), true); children.back()=false;
            ui.showSubmenu("Pending notes", entries, pending_index, children);
            break;
        }
        case MenuView::CancelPending:
            ui.showCancelConfirmation(cancel_selected,cancel_all,cancel_id);
            break;
        case MenuView::Wifi:
            ui.showSubmenu("Wi-Fi", {"ESP32 IP", "Saved networks", "Start portal", "Reconnect", "Open web settings", "Back"}, submenu_index,
                           {true, true, true, false, true, false});
            break;
        case MenuView::Storage: {
            const auto usage=!web.downloadBusy() && sd_mounted && recorder.waitStopped(0) && preview.waitStopped(0) ? store.usage() : storage::Usage{};
            ui.showStorageMenu(submenu_index,usage.known,usage.total,usage.free);
            break;
        }
        case MenuView::Format:
            ui.showFormatConfirmation(erase_selected);
            break;
        case MenuView::Portal:
            ui.showPortal(portal.ssid(), portal.password(), portal.address());
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
            if (info_title=="Storage") {
                const auto usage=!web.downloadBusy() && sd_mounted && recorder.waitStopped(0) && preview.waitStopped(0) ? store.usage() : storage::Usage{};
                ui.showStorageStatus(info_message,usage.known,usage.total,usage.free);
            } else ui.showInfo(info_title, info_message);
            break;
    }
}

void showMenuInfo(const std::string &title, const std::string &message) {
    info_parent = menu_view == MenuView::Format ? MenuView::Storage :
        menu_view == MenuView::CancelPending ? (cancel_all ? MenuView::Synchronization : MenuView::Pending) : menu_view;
    info_title = title;
    info_message = message;
    menu_view = MenuView::Info;
    renderMenu();
}

void reconnectWifi() {
    if (portal.active()) return;
    last_wifi_attempt_us = esp_timer_get_time();
    publishWeb(true);
    wifi.connectPreferred();
    publishWeb();
}

void executeMenuItem() {
    if(web.downloadBusy()) return; // No card reads/mutations or worker launch during HTTP lease.
    if (menu_view == MenuView::CancelPending) {
        const MenuView parent = cancel_all ? MenuView::Synchronization : MenuView::Pending;
        if (!cancel_selected) { menu_view=parent; renderMenu(); return; }
        cancel_selected=false;
        if (state!=AppState::Menu || !sd_mounted || stopping_recording || portal.active() ||
            !recorder.waitStopped(0) || !preview.waitStopped(0)) {
            showMenuInfo("Cancellation unavailable", "Wait for stopped workers and a mounted SD. Pending notes retained."); return;
        }
        const auto ids = cancel_all ? store.pendingIds() : std::vector<std::string>{cancel_id};
        size_t cancelled=0;
        for (const auto &id : ids) {
            if (store.cancelPending(id)) {
                ++cancelled;
                if (last_note_id==id) last_note_text="Audio saved. Transcription cancelled.";
            }
        }
        pending_notes=store.pendingIds(); pending_index=std::min(pending_index,pending_notes.size());
        saved_notes.clear(); reader_text.clear(); note_index=reader_page=0;
        showMenuInfo(cancelled==ids.size() ? "Cancellation complete" : "Cancellation incomplete",
            "Audio retained. Cancelled: " + std::to_string(cancelled) + "\nStill pending: " + std::to_string(store.pendingCount()));
        publishWeb(); return;
    }
    if (menu_view == MenuView::Pending) {
        if (pending_index>=pending_notes.size()) { menu_view=MenuView::Synchronization; renderMenu(); return; }
        cancel_id=pending_notes[pending_index]; cancel_all=false; cancel_selected=false;
        menu_view=MenuView::CancelPending; renderMenu(); publishWeb(); return;
    }
    if (menu_view == MenuView::Synchronization) {
        if (submenu_index==3) { menu_view=MenuView::Settings; renderMenu(); }
        else if (submenu_index==1) {
            pending_notes=store.pendingIds(); pending_index=0; menu_view=MenuView::Pending; renderMenu();
        } else if (submenu_index==2) {
            cancel_all=true; cancel_selected=false; cancel_id.clear(); menu_view=MenuView::CancelPending; renderMenu(); publishWeb();
        } else {
            if (!wifi.connected()) reconnectWifi();
            if (!wifi.connected()) showMenuInfo("Synchronization", "No network. Notes retained on SD.");
            else if (!store.pendingCount()) showMenuInfo("Synchronization", "No pending notes.");
            else {
                last_sync_attempt_us=esp_timer_get_time();
                const bool ok=syncPending();
                showMenuInfo("Synchronization", std::string(sync_cancelled ? "Sync cancelled. Pending audio kept. Use Sync now to retry." : ok ? "Sync finished." : "Sync failed.") +
                    "\nPending notes: " + std::to_string(store.pendingCount()));
            }
        }
        return;
    }
    if (menu_view == MenuView::Portal) {
        if (!portal.stop()) {
            menu_view=MenuView::Wifi;
            showMenuInfo("Restart required", "Portal cleanup failed. Restart before using Wi-Fi again.");
            return;
        }
        if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
        menu_view = MenuView::Wifi;
        reconnectWifi();
        renderMenu();
        return;
    }
    if (menu_view == MenuView::Wifi) {
        switch (submenu_index) {
            case 0: showMenuInfo("ESP32 IP", wifi.connected() && !wifi.ipAddress().empty()
                ? "IP: " + wifi.ipAddress() + "\nSSID: " + wifi.currentSsid()
                : "Offline. No station IP address."); break;
            case 1: {
                const auto ssids = wifi.savedSsids();
                std::string message;
                for (size_t i=0; i<ssids.size(); ++i)
                    message += std::string(i==0 ? "Home: " : "Hotspot: ") + (ssids[i].empty() ? "Disabled" : ssids[i]) + "\n";
                showMenuInfo("Saved Wi-Fi", message);
                break;
            }
            case 2:
                if (stopping_recording || !recorder.waitStopped(0) || !preview.waitStopped(0)) {
                    showMenuInfo("Portal", "Workers still stopping. Return to retry."); break;
                }
                web.stop();
                if (!portal.start(wifi)) {
                    if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
                    reconnectWifi();
                    showMenuInfo("Portal failed", "Configuration portal unavailable. Return to retry.");
                } else {
                    menu_view=MenuView::Portal;
                    publishWeb(); renderMenu();
                }
                break;
            case 3: reconnectWifi(); showMenuInfo("Wi-Fi", wifi.connected()
                ? "Connected. IP: " + wifi.ipAddress() : "No network connected."); break;
            case 4: {
                if (!wifi.connected() || stopping_recording || !recorder.waitStopped(0) || !preview.waitStopped(0)) {
                    showMenuInfo("Web unavailable","Connect station Wi-Fi and wait for stopped workers."); break;
                }
                publishWeb();
                const auto code=web.openLocalSession();
                if (code.empty()) showMenuInfo("Web unavailable","Local server is not running.");
                else showMenuInfo("Web settings","http://"+wifi.ipAddress()+"\nPIN: "+code+"\nValid 10 minutes.\n5 incorrect attempts\nrequire a new PIN.");
                break;
            }
            case 5: menu_view=MenuView::Settings; renderMenu(); break;
        }
        return;
    }
    if (menu_view == MenuView::Storage) {
        if (submenu_index==0) {
            if (!sd_mounted) sd_mounted=store.init();
            showMenuInfo("Storage", std::string(sd_mounted ? "SD mounted." : store.lastError()) +
                "\nPending notes: " + std::to_string(store.pendingCount()) +
                (audio_ready ? "" : "\nAudio unavailable."));
        } else if (submenu_index==1) {
            erase_selected=false; menu_view=MenuView::Format; renderMenu(); publishWeb();
        } else { menu_view=MenuView::Settings; renderMenu(); }
        return;
    }
    if (menu_view == MenuView::Format) {
        if (!erase_selected) { menu_view=MenuView::Storage; renderMenu(); return; }
        erase_selected=false;
        if (state!=AppState::Menu || !sd_mounted || stopping_recording || !recorder.waitStopped(0) || !preview.waitStopped(0) || portal.active()) {
            showMenuInfo("Format unavailable", "Requires a mounted FAT card and stopped workers. No data erased."); return;
        }
        // Main owns all SD operations; invalidate cached card data even on uncertain failure.
        saved_notes.clear(); reader_text.clear(); active_note_id.clear();
        last_note_id.clear(); last_note_text.clear(); live_text.clear();
        note_index=reader_page=0;
        sd_mounted=store.format();
        recording_recovery_required=!sd_mounted;
        showMenuInfo(sd_mounted ? "SD formatted" : "Format failed", sd_mounted
            ? "All card data erased. Note directories recreated."
            : "SD unavailable. Data may be erased. Retry mount or restart; recover the card externally if needed.");
        publishWeb(); return;
    }
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
        menu_view = info_parent;
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
        case 2:
            submenu_index=0; menu_view=MenuView::Wifi; renderMenu();
            break;
        case 1:
            submenu_index=0; menu_view=MenuView::Synchronization; renderMenu();
            break;
        case 3:
            submenu_index=0; menu_view=MenuView::Storage; renderMenu();
            break;
        case 5:
            showMenuInfo("About", "Waveshare ESP32-S3\nePaper 1.54 V2\n200x200 black/white\nVoice Notes / Whistle");
            break;
        case 4:
            for (size_t i = 0; i < sizeof(REFRESH_OPTIONS); ++i)
                if (REFRESH_OPTIONS[i] == refresh_limit) refresh_option = i;
            menu_view = MenuView::Refresh;
            renderMenu();
            break;
        case 6:
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

void pollPortal() {
    if (!portal.active()) return;
    portal.pollScan();
    if (portal.expired()) {
        if (!portal.stop()) {
            menu_view=MenuView::Wifi;
            showMenuInfo("Restart required", "Portal timeout cleanup failed. Restart before using Wi-Fi again.");
            return;
        }
        if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
        menu_view=MenuView::Wifi;
        reconnectWifi();
        showMenuInfo("Portal closed", "Configuration timed out. Station Wi-Fi resumed.");
        return;
    }
    network::WifiProfiles profiles;
    if (!portal.takeProfiles(profiles)) return;
    const bool ok=state==AppState::Menu && (menu_view==MenuView::Portal ||
        (menu_view==MenuView::Info && info_parent==MenuView::Portal)) &&
        !stopping_recording && recorder.waitStopped(0) && preview.waitStopped(0) && wifi.saveProfiles(profiles);
    portal.complete(ok);
    for (auto &profile : profiles) { profile.password.clear(); }
    if (!ok) {
        // Keep the AP available for retry, with an honest physical completion result.
        menu_view=MenuView::Portal;
        showMenuInfo("Wi-Fi save failed", "Profiles not activated. Portal still open. Return to setup and retry the form.");
        return;
    }
    if (!portal.stop()) {
        menu_view=MenuView::Wifi;
        showMenuInfo("Restart required", "Profiles saved, but portal cleanup failed. Restart before using Wi-Fi again.");
        return;
    }
    if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
    menu_view=MenuView::Wifi;
    reconnectWifi();
    showMenuInfo("Wi-Fi saved", wifi.connected() ? "Profiles saved. IP: " + wifi.ipAddress()
        : "Profiles saved. Connection failed. Return to restart the portal or reconnect.");
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
            if ((menu_view == MenuView::Info || menu_view == MenuView::Portal) &&
                (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress)) {
                executeMenuItem(); return;
            }
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress) {
                if (menu_view == MenuView::Settings) {
                    menu_index = (menu_index + 1) % display::Ui::MENU_ITEMS;
                } else if (menu_view == MenuView::Refresh) {
                    refresh_option = (refresh_option + 1) % sizeof(REFRESH_OPTIONS);
                } else if (menu_view == MenuView::Pending) {
                    pending_index=(pending_index+1) % (pending_notes.size()+1);
                } else if (menu_view == MenuView::CancelPending) {
                    cancel_selected=!cancel_selected;
                } else if (menu_view == MenuView::Synchronization) {
                    submenu_index=(submenu_index+1) % 4;
                } else if (menu_view == MenuView::Wifi || menu_view == MenuView::Storage) {
                    submenu_index=(submenu_index+1) % (menu_view==MenuView::Wifi ? 6 : 3);
                } else if (menu_view == MenuView::Format) {
                    erase_selected=!erase_selected;
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
            if (event == app::ButtonEvent::LongPress && !sync_stop.exchange(true)) {
                ui.setStatus(wifi.connected(), sd_mounted);
                ui.showSyncStopping();
            }
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

    ui.setStatus(false, sd_mounted);
    ui.showBootProgress(1, "SD card", sd_mounted ? "Ready" : "Unavailable. Settings can retry.");
    ui.setStatus(false, sd_mounted);
    ui.showBootProgress(1, "Audio", "Initializing");
    audio_ready = recorder.init();
    ui.setStatus(false, sd_mounted);
    ui.showBootProgress(2, "Audio", audio_ready ? "Ready" : "Unavailable. Recording disabled.");
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
    ui.setStatus(wifi.connected(), sd_mounted);
    std::string server_detail = "Skipped: no Wi-Fi";
    if (wifi.connected()) {
        ui.showBootProgress(3, "Server", "Checking /health");
        const auto health = api.health();
        server_detail = health == network::HealthStatus::Ready ? "Ready" :
            health == network::HealthStatus::Skipped ? "Skipped: not configured" : "Unavailable. Sync can retry.";
        ui.setStatus(wifi.connected(), sd_mounted);
    }
    ui.showBootProgress(4, "Server", server_detail);
    last_wifi_attempt_us = esp_timer_get_time();
    renderIdle();
    if (!web.start()) ESP_LOGW(TAG, "Local web server unavailable");
    publishWeb();

    if (wifi.connected() && store.pendingCount() > 0) {
        syncPending();
    }

    while (true) {
        pollPortal();
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

        if (!auto_sync_paused && !web.downloadBusy() && wifi.connected() && store.pendingCount() > 0 &&
            (now - last_sync_attempt_us) / 1000 >= config::SYNC_RETRY_MS) {
            last_sync_attempt_us = now;
            syncPending();
        }
        publishWeb();
    }
}
