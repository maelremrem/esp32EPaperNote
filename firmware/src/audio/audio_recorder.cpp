#include "audio/audio_recorder.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "audio/wav_writer.h"
#include "board_pins.h"
#include "project_config.h"
#include "driver/gpio.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace audio {

static const char *TAG = "audio";

bool AudioRecorder::init() {
    gpio_config_t power{};
    power.pin_bit_mask = 1ULL << board::AUDIO_PWR;
    power.mode = GPIO_MODE_OUTPUT;
    power.pull_up_en = GPIO_PULLUP_DISABLE;
    power.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&power));
    gpio_set_level(board::AUDIO_PWR, 0); // V2 supply enable is active LOW.
    vTaskDelay(pdMS_TO_TICKS(30));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(board::I2S_PORT, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, nullptr, &rx_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    i2s_std_config_t std_cfg{};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config::AUDIO_SAMPLE_RATE);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.gpio_cfg.mclk = board::I2S_MCLK;
    std_cfg.gpio_cfg.bclk = board::I2S_BCLK;
    std_cfg.gpio_cfg.ws = board::I2S_LRCK;
    std_cfg.gpio_cfg.dout = GPIO_NUM_NC;
    std_cfg.gpio_cfg.din = board::I2S_DIN;
    std_cfg.gpio_cfg.invert_flags = {false, false, false};

    if (i2s_channel_init_std_mode(rx_handle_, &std_cfg) != ESP_OK ||
        i2s_channel_enable(rx_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX init failed");
        return false;
    }

    i2c_master_bus_config_t i2c_cfg{};
    i2c_cfg.i2c_port = board::I2C_PORT;
    i2c_cfg.sda_io_num = board::I2C_SDA;
    i2c_cfg.scl_io_num = board::I2C_SCL;
    i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_cfg.glitch_ignore_cnt = 7;
    i2c_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&i2c_cfg, &i2c_bus_) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return false;
    }

    audio_codec_i2c_cfg_t codec_i2c{};
    codec_i2c.port = board::I2C_PORT;
    codec_i2c.addr = ES8311_CODEC_DEFAULT_ADDR;
    codec_i2c.bus_handle = i2c_bus_;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "codec I2C interface failed");
        return false;
    }

    audio_codec_i2s_cfg_t codec_i2s{};
    codec_i2s.port = board::I2S_PORT;
    codec_i2s.rx_handle = rx_handle_;
    codec_i2s.tx_handle = nullptr;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&codec_i2s);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!data_if || !gpio_if) {
        ESP_LOGE(TAG, "codec data/gpio interface failed");
        return false;
    }

    es8311_codec_cfg_t es_cfg{};
    es_cfg.ctrl_if = ctrl_if;
    es_cfg.gpio_if = gpio_if;
    es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC;
    es_cfg.master_mode = false;
    es_cfg.use_mclk = true;
    es_cfg.pa_pin = GPIO_NUM_NC;
    es_cfg.pa_reverted = false;
    es_cfg.mclk_div = I2S_MCLK_MULTIPLE_256;
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 interface failed");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg{};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = codec_if;
    dev_cfg.data_if = data_if;
    codec_ = esp_codec_dev_new(&dev_cfg);
    if (!codec_) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return false;
    }

    esp_codec_dev_sample_info_t sample{};
    sample.bits_per_sample = 16;
    sample.channel = 1;
    sample.channel_mask = 0x01;
    sample.sample_rate = config::AUDIO_SAMPLE_RATE;
    sample.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    if (esp_codec_dev_open(codec_, &sample) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return false;
    }
    esp_codec_dev_set_in_gain(codec_, config::MIC_GAIN_DB);

    finished_sem_ = xSemaphoreCreateBinary();
    return finished_sem_ != nullptr;
}

bool AudioRecorder::start(const std::string &path, PcmSink *preview) {
    if (active_ || !codec_ || !finished_sem_) {
        return false;
    }
    path_ = path;
    preview_ = preview;
    active_ = true;
    stop_requested_ = false;
    recorded_bytes_ = 0;
    saved_cleanly_ = false;
    recording_ = true;

    while (xSemaphoreTake(finished_sem_, 0) == pdTRUE) {}

    if (xTaskCreate(taskEntry, "record_audio", 6144, this, 8, nullptr) != pdPASS) {
        recording_ = false;
        active_ = false;
        return false;
    }
    return true;
}

void AudioRecorder::requestStop() {
    stop_requested_ = true;
}

bool AudioRecorder::waitStopped(uint32_t timeout_ms) {
    if (!active_) {
        return true;
    }
    if (xSemaphoreTake(finished_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    active_ = false;
    preview_ = nullptr;
    return true;
}

void AudioRecorder::taskEntry(void *arg) {
    static_cast<AudioRecorder *>(arg)->recordTask();
    vTaskDelete(nullptr);
}

void AudioRecorder::recordTask() {
    FILE *file = std::fopen(path_.c_str(), "wb+");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open %s", path_.c_str());
        recording_ = false;
        xSemaphoreGive(finished_sem_);
        return;
    }

    bool wav_ok = writeWavHeader(file, 0, config::AUDIO_SAMPLE_RATE, config::AUDIO_CHANNELS, config::AUDIO_BITS);
    if (std::fseek(file, 44, SEEK_SET) != 0) wav_ok = false;

    alignas(4) uint8_t buffer[config::AUDIO_READ_CHUNK];
    while (!stop_requested_) {
        const int ret = esp_codec_dev_read(codec_, buffer, sizeof(buffer));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Audio read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        const size_t written = std::fwrite(buffer, 1, sizeof(buffer), file);
        recorded_bytes_ += static_cast<uint32_t>(written);
        if (written != sizeof(buffer)) {
            ESP_LOGE(TAG, "SD write failed during recording");
            wav_ok = false;
            break;
        }
        if (preview_) preview_->tryPush(buffer, written);
    }

    if (!writeWavHeader(file, recorded_bytes_.load(), config::AUDIO_SAMPLE_RATE,
                        config::AUDIO_CHANNELS, config::AUDIO_BITS)) wav_ok = false;
    if (std::fflush(file) != 0) wav_ok = false;
    if (fsync(fileno(file)) != 0) wav_ok = false;
    if (std::fclose(file) != 0) wav_ok = false;
    saved_cleanly_ = wav_ok;

    recording_ = false;
    stop_requested_ = false;
    ESP_LOGI(TAG, "Recording complete: %lu PCM bytes", static_cast<unsigned long>(recorded_bytes_.load()));
    xSemaphoreGive(finished_sem_);
}

} // namespace audio
