#include "network/live_preview.h"
#include <cstring>
#include "network/api_client.h"
#include "network/live_preview_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/task.h"

namespace network {
void LivePreview::release() {
    if (queue_) vQueueDelete(queue_);
    if (done_) vSemaphoreDelete(done_);
    if (mutex_) vSemaphoreDelete(mutex_);
    heap_caps_free(window_);
    queue_ = nullptr;
    done_ = mutex_ = nullptr;
    window_ = nullptr;
    text_.clear();
    note_id_.clear();
}

bool LivePreview::start(const std::string &note_id) {
    if (active_ || live::endpoint(note_id).empty()) return false;
    queue_ = xQueueCreate(config::LIVE_QUEUE_CHUNKS, sizeof(Chunk));
    done_ = xSemaphoreCreateBinary();
    mutex_ = xSemaphoreCreateMutex();
    window_ = static_cast<uint8_t *>(heap_caps_malloc(config::LIVE_WINDOW_BYTES,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!window_) window_ = static_cast<uint8_t *>(heap_caps_malloc(
        config::LIVE_WINDOW_BYTES, MALLOC_CAP_8BIT));
    if (!queue_ || !done_ || !mutex_ || !window_) { release(); return false; }
    note_id_ = note_id;
    text_.clear();
    text_.reserve(config::LIVE_TEXT_MAX);
    sequence_ = 0;
    cancelled_.store(false);
    active_ = true;
    if (xTaskCreate(taskEntry, "live_preview", 8192, this, 3, nullptr) != pdPASS) {
        cancelled_.store(true);
        active_ = false;
        release();
        return false;
    }
    return true;
}

void LivePreview::tryPush(const uint8_t *pcm, size_t bytes) {
    if (cancelled_.load() || bytes != config::AUDIO_READ_CHUNK) return;
    producer_chunk_.sequence = sequence_++;
    std::memcpy(producer_chunk_.pcm, pcm, bytes);
    // Copy by value. Never retain the recorder's stack buffer, never wait for Wi-Fi.
    xQueueSend(queue_, &producer_chunk_, 0);
}

bool LivePreview::snapshot(std::string &text) {
    if (!active_) return false;
    if (xSemaphoreTake(mutex_, 0) == pdTRUE) {
        text = text_;
        xSemaphoreGive(mutex_);
        return true;
    }
    return false;
}

bool LivePreview::waitStopped(uint32_t timeout_ms) {
    if (!active_) return true;
    if (xSemaphoreTake(done_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    active_ = false;
    release();
    return true;
}

void LivePreview::taskEntry(void *arg) {
    auto *self = static_cast<LivePreview *>(arg);
    self->run();
    // No access to self or its buffers after signaling completion.
    xSemaphoreGive(self->done_);
    vTaskDelete(nullptr);
}

void LivePreview::run() {
    ApiClient api;
    live::PcmWindow window(window_, config::LIVE_WINDOW_BYTES);
    Chunk chunk;
    while (!cancelled_.load()) {
        if (xQueueReceive(queue_, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (cancelled_.load()) break;
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { window.reset(); continue; }
        // Queue overflow means a discontinuity: start a fresh contiguous window.
        if (!window.append(chunk.sequence, chunk.pcm, sizeof(chunk.pcm))) { window.reset(); continue; }
        if (window.size() < config::LIVE_WINDOW_BYTES) continue;
        const auto result = api.preview(note_id_, window_, window.size(), cancelled_);
        window.reset(); // Non-overlapping windows; preview never affects the WAV or note store.
        if (result.ok && !cancelled_.load()) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            live::appendText(text_, result.text, config::LIVE_TEXT_MAX);
            xSemaphoreGive(mutex_);
        } else if (!cancelled_.load()) {
            ESP_LOGW("live_preview", "Preview unavailable (HTTP %d)", result.http_status);
        }
        // Drop stale PCM captured during network work; prefer current speech over backlog.
        xQueueReset(queue_);
    }
}
}
