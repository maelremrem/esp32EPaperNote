#include <string>

#include "app/button.h"
#include "audio/audio_recorder.h"
#include "display/epaper_display.h"
#include "display/ui.h"
#include "network/api_client.h"
#include "network/wifi_manager.h"
#include "project_config.h"
#include "storage/note_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace {

const char *TAG = "voice_notes";

enum class AppState {
    Idle,
    Recording,
    Menu,
    Syncing,
};

display::EpaperDisplay epaper;
display::Ui ui(epaper);
storage::NoteStore store;
audio::AudioRecorder recorder;
network::WifiManager wifi;
network::ApiClient api;
app::Button button;

AppState state = AppState::Idle;
std::string active_note_id;
std::string last_note_id;
std::string last_note_text;
size_t menu_index = 0;
int64_t last_wifi_attempt_us = 0;
int64_t last_sync_attempt_us = 0;

void renderIdle() {
    const size_t pending = store.pendingCount();
    if (!wifi.connected() && pending > 0) {
        ui.showOffline(pending, last_note_id, last_note_text);
    } else {
        ui.showIdle(last_note_id, last_note_text, pending, wifi.connected());
    }
}

bool syncPending() {
    if (!wifi.connected()) {
        renderIdle();
        return false;
    }

    auto pending = store.pendingIds();
    if (pending.empty()) {
        renderIdle();
        return true;
    }

    state = AppState::Syncing;
    bool all_ok = true;

    for (size_t i = 0; i < pending.size(); ++i) {
        const std::string &id = pending[i];
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
    }

    state = AppState::Idle;
    renderIdle();
    return all_ok;
}

void startRecording() {
    if (state == AppState::Recording) {
        return;
    }
    active_note_id = store.makeNoteId();
    if (!recorder.start(store.recordingTempPath())) {
        ui.showError("AUDIO", "Impossible de demarrer l'enregistrement");
        state = AppState::Idle;
        return;
    }
    state = AppState::Recording;
    ui.showRecording(active_note_id);
}

void stopRecording() {
    if (state != AppState::Recording) {
        return;
    }
    recorder.requestStop();
    if (!recorder.waitStopped(10000)) {
        ui.showError("AUDIO", "Timeout pendant la sauvegarde");
        state = AppState::Idle;
        return;
    }

    if (recorder.recordedBytes() < 3200) {
        store.discardRecording();
        active_note_id.clear();
        ui.showError("NOTE TROP COURTE", "Aucun audio utile enregistre");
        state = AppState::Idle;
        return;
    }

    if (!store.commitRecording(active_note_id)) {
        ui.showError("SD", "La note n'a pas pu etre finalisee");
        state = AppState::Idle;
        return;
    }

    last_note_id = active_note_id;
    last_note_text = "Audio sauvegarde. Transcription en attente.";
    active_note_id.clear();
    state = AppState::Idle;

    if (wifi.connected()) {
        syncPending();
    } else {
        renderIdle();
    }
}

void executeMenuItem() {
    const size_t selected = menu_index;
    menu_index = 0;
    state = AppState::Idle;

    switch (selected) {
        case 0:
            startRecording();
            break;
        case 1:
            syncPending();
            break;
        case 2:
            // MVP: history navigation will be implemented from notes/index.jsonl.
            renderIdle();
            break;
        case 3:
            wifi.connectPreferred();
            renderIdle();
            break;
        default:
            renderIdle();
            break;
    }
}

void handleButton(app::ButtonEvent event) {
    switch (state) {
        case AppState::Recording:
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::LongPress) {
                stopRecording();
            }
            return;

        case AppState::Menu:
            if (event == app::ButtonEvent::ShortPress || event == app::ButtonEvent::DoublePress) {
                menu_index = (menu_index + 1) % 4;
                ui.showMenu(menu_index);
            } else if (event == app::ButtonEvent::LongPress) {
                executeMenuItem();
            }
            return;

        case AppState::Syncing:
            // Synchronization is deliberately non-interruptible in the MVP.
            return;

        case AppState::Idle:
            if (event == app::ButtonEvent::DoublePress) {
                startRecording();
            } else if (event == app::ButtonEvent::LongPress) {
                if (!wifi.connected() && store.pendingCount() > 0) {
                    last_wifi_attempt_us = esp_timer_get_time();
                    if (wifi.connectPreferred()) {
                        syncPending();
                    } else {
                        renderIdle();
                    }
                } else {
                    state = AppState::Menu;
                    menu_index = 0;
                    ui.showMenu(menu_index);
                }
            } else {
                // History browsing is the next UI milestone. For now, refresh the current view.
                renderIdle();
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

    if (!epaper.init()) {
        ESP_LOGE(TAG, "ePaper init failed");
        return;
    }
    ui.showBoot();

    if (!store.init()) {
        ui.showError("SD ERROR", "Carte SD absente ou non FAT32");
        return;
    }

    if (!recorder.init()) {
        ui.showError("AUDIO ERROR", "ES8311 ou I2S indisponible");
        return;
    }

    if (!button.init()) {
        ui.showError("BUTTON ERROR", "GPIO bouton indisponible");
        return;
    }

    wifi.init();
    wifi.connectPreferred();
    last_wifi_attempt_us = esp_timer_get_time();
    renderIdle();

    if (wifi.connected() && store.pendingCount() > 0) {
        syncPending();
    }

    while (true) {
        app::ButtonEvent event{};
        if (xQueueReceive(button.queue(), &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            handleButton(event);
        }

        if (state != AppState::Idle) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        if (!wifi.connected() &&
            (now - last_wifi_attempt_us) / 1000 >= config::WIFI_RETRY_IDLE_MS) {
            last_wifi_attempt_us = now;
            if (wifi.connectPreferred()) {
                renderIdle();
            }
        }

        if (wifi.connected() && store.pendingCount() > 0 &&
            (now - last_sync_attempt_us) / 1000 >= config::SYNC_RETRY_MS) {
            last_sync_attempt_us = now;
            syncPending();
        }
    }
}
