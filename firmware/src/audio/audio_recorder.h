#pragma once

#include <string>
#include <atomic>
#include "audio/pcm_sink.h"

#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace audio {

class AudioRecorder {
public:
    bool init();
    bool start(const std::string &path, PcmSink *preview = nullptr);
    void requestStop();
    bool waitStopped(uint32_t timeout_ms);
    bool isRecording() const { return recording_.load(); }
    uint32_t recordedBytes() const { return recorded_bytes_.load(); }
    bool savedCleanly() const { return saved_cleanly_.load(); }

private:
    static void taskEntry(void *arg);
    void recordTask();

    i2s_chan_handle_t rx_handle_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    SemaphoreHandle_t finished_sem_ = nullptr;

    std::string path_;
    PcmSink *preview_ = nullptr; // Immutable while task is active; main joins before freeing.
    bool active_ = false; // Main task only; cleared after completion semaphore is consumed.
    std::atomic<bool> recording_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<uint32_t> recorded_bytes_{0};
    std::atomic<bool> saved_cleanly_{false};
};

} // namespace audio
