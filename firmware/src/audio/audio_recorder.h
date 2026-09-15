#pragma once

#include <string>

#include "esp_codec_dev.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace audio {

class AudioRecorder {
public:
    bool init();
    bool start(const std::string &path);
    void requestStop();
    bool waitStopped(uint32_t timeout_ms);
    bool isRecording() const { return recording_; }
    uint32_t recordedBytes() const { return recorded_bytes_; }

private:
    static void taskEntry(void *arg);
    void recordTask();

    i2s_chan_handle_t rx_handle_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    SemaphoreHandle_t finished_sem_ = nullptr;

    std::string path_;
    volatile bool recording_ = false;
    volatile bool stop_requested_ = false;
    volatile uint32_t recorded_bytes_ = 0;
};

} // namespace audio
