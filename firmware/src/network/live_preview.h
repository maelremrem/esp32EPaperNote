#pragma once
#include <atomic>
#include <string>
#include "audio/pcm_sink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "project_config.h"

namespace network {
// Application owns this for its lifetime. No task is force-deleted on stop.
class LivePreview : public audio::PcmSink {
public:
    bool start(const std::string &note_id);
    void tryPush(const uint8_t *pcm, size_t bytes) override;
    void requestStop() { cancelled_.store(true); }
    // Call only AFTER recorder.waitStopped() succeeds: producer must be joined too.
    bool waitStopped(uint32_t timeout_ms);
    bool snapshot(std::string &text);
private:
    struct Chunk {
        uint32_t sequence;
        uint8_t pcm[config::AUDIO_READ_CHUNK];
    };
    Chunk producer_chunk_{}; // Producer-only scratch; don't double recorder's stack usage.
    static void taskEntry(void *arg);
    void run();
    void release();
    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    uint8_t *window_ = nullptr;
    std::string note_id_;
    std::string text_;
    uint32_t sequence_ = 0; // Only producer writes; reset before it starts.
    bool active_ = false; // Only main task accesses.
    std::atomic<bool> cancelled_{true};
};
}
