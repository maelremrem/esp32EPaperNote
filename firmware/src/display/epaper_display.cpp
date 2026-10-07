#include "display/epaper_display.h"

#include <algorithm>
#include <cstring>

#include "board_pins.h"
#include "display/font8x12.h"
#include "display/epaper_waveforms.h"
#include "display/text_layout.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace display {

static const char *TAG = "epaper";

bool EpaperDisplay::init() {
    gpio_config_t out{};
    out.pin_bit_mask = (1ULL << board::EPD_PWR) |
                       (1ULL << board::EPD_RST) |
                       (1ULL << board::EPD_DC) |
                       (1ULL << board::EPD_CS);
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&out) != ESP_OK) {
        return false;
    }

    gpio_config_t in{};
    in.pin_bit_mask = 1ULL << board::EPD_BUSY;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&in) != ESP_OK) {
        return false;
    }

    gpio_set_level(board::EPD_PWR, 0); // V2 supply enable is active LOW.
    gpio_set_level(board::EPD_CS, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    spi_bus_config_t bus{};
    bus.mosi_io_num = board::EPD_MOSI;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = board::EPD_SCLK;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = sizeof(framebuffer_);
    esp_err_t err = spi_bus_initialize(board::EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev{};
    dev.mode = 0;
    dev.clock_speed_hz = 10 * 1000 * 1000;
    dev.spics_io_num = -1;
    dev.queue_size = 4;
    if (spi_bus_add_device(board::EPD_SPI_HOST, &dev, &spi_) != ESP_OK) {
        ESP_LOGE(TAG, "SPI device init failed");
        return false;
    }

    hardwareReset();
    controllerInit();
    clear();
    reference_valid_ = false;
    partial_count_ = 0;
    needs_reset_ = !io_ok_;
    return io_ok_;
}

void EpaperDisplay::hardwareReset() {
    gpio_set_level(board::EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(board::EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(board::EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool EpaperDisplay::waitBusy(uint32_t timeout_ms) {
    if (!io_ok_) return false;
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    // Waveshare V2 examples use HIGH = busy, LOW = idle.
    while (gpio_get_level(board::EPD_BUSY) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "BUSY timeout");
            io_ok_ = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void EpaperDisplay::controllerInit() {
    if (!waitBusy()) return;
    sendCommand(0x12); // software reset
    waitBusy();

    sendCommand(0x01); // driver output control: 200 lines
    sendData(0xC7);
    sendData(0x00);
    sendData(0x00);

    sendCommand(0x11); // data entry: X+, Y+
    sendData(0x03);

    sendCommand(0x44); // X RAM window: 0..24 bytes
    sendData(0x00);
    sendData(0x18);

    sendCommand(0x45); // Y RAM window: 0..199
    sendData(0x00);
    sendData(0x00);
    sendData(0xC7);
    sendData(0x00);

    sendCommand(0x3C); // border waveform
    sendData(0x01);

    sendCommand(0x18); // use internal temperature sensor
    sendData(0x80);
    sendCommand(0x22); // vendor temperature/waveform loading preparation
    sendData(0xB1);
    sendCommand(0x20);
    waitBusy();

    sendCommand(0x4E);
    sendData(0x00);
    sendCommand(0x4F);
    sendData(0x00);
    sendData(0x00);
    waitBusy();
}

void EpaperDisplay::loadLut(const uint8_t *lut) {
    sendCommand(0x32);
    sendBuffer(lut, 153);
    waitBusy();
    sendCommand(0x3F); sendData(lut[153]);
    sendCommand(0x03); sendData(lut[154]);
    sendCommand(0x04); sendBuffer(lut + 155, 3);
    sendCommand(0x2C); sendData(lut[158]);
}

void EpaperDisplay::sendCommand(uint8_t value) {
    if (!io_ok_ || !spi_) return;
    gpio_set_level(board::EPD_DC, 0);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &value;
    io_ok_ = spi_device_polling_transmit(spi_, &t) == ESP_OK;
    if (!io_ok_) ESP_LOGE(TAG, "SPI transfer failed");
    gpio_set_level(board::EPD_CS, 1);
}

void EpaperDisplay::sendData(uint8_t value) {
    if (!io_ok_ || !spi_) return;
    gpio_set_level(board::EPD_DC, 1);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &value;
    io_ok_ = spi_device_polling_transmit(spi_, &t) == ESP_OK;
    if (!io_ok_) ESP_LOGE(TAG, "SPI transfer failed");
    gpio_set_level(board::EPD_CS, 1);
}

void EpaperDisplay::sendBuffer(const uint8_t *data, size_t len) {
    if (!io_ok_ || !spi_) return;
    gpio_set_level(board::EPD_DC, 1);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = len * 8;
    t.tx_buffer = data;
    io_ok_ = spi_device_polling_transmit(spi_, &t) == ESP_OK;
    if (!io_ok_) ESP_LOGE(TAG, "SPI transfer failed");
    gpio_set_level(board::EPD_CS, 1);
}

void EpaperDisplay::clear(bool white) {
    std::memset(framebuffer_, white ? 0xFF : 0x00, sizeof(framebuffer_));
}

void EpaperDisplay::drawPixel(int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= width() || y >= height()) {
        return;
    }
    const size_t index = static_cast<size_t>(x / 8 + y * (width() / 8));
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    if (black) {
        framebuffer_[index] &= static_cast<uint8_t>(~mask);
    } else {
        framebuffer_[index] |= mask;
    }
}

void EpaperDisplay::drawHLine(int x, int y, int w, bool black) {
    for (int i = 0; i < w; ++i) drawPixel(x + i, y, black);
}

void EpaperDisplay::drawVLine(int x, int y, int h, bool black) {
    for (int i = 0; i < h; ++i) drawPixel(x, y + i, black);
}

void EpaperDisplay::drawRect(int x, int y, int w, int h, bool black) {
    drawHLine(x, y, w, black);
    drawHLine(x, y + h - 1, w, black);
    drawVLine(x, y, h, black);
    drawVLine(x + w - 1, y, h, black);
}

void EpaperDisplay::fillRect(int x, int y, int w, int h, bool black) {
    for (int yy = 0; yy < h; ++yy) drawHLine(x, y + yy, w, black);
}

char EpaperDisplay::transliterateUtf8(const char *&p) const {
    return text::nextGlyph(p);
}

void EpaperDisplay::drawText(int x, int y, const std::string &text, int scale, bool black) {
    // Never paint half a glyph. UI callers pre-wrap to their own content box;
    // legacy callers retain implicit panel-width wrapping below.
    if (scale <= 0 || scale > width() / font::WIDTH || x < 0 || y < 0 ||
        x > width() - font::WIDTH * scale) return;
    const char *p = text.c_str();
    int cursor_x = x;
    int cursor_y = y;
    while (*p) {
        if (cursor_y > height() - font::HEIGHT * scale) break;
        char c = transliterateUtf8(p);
        if (c == '\n') {
            cursor_x = x;
            cursor_y += (font::HEIGHT + 2) * scale;
            continue;
        }
        if (c < 32 || c > 126) c = '?';
        const auto &glyph = font::GLYPHS[static_cast<unsigned char>(c) - 32];
        for (int gy = 0; gy < font::HEIGHT; ++gy) {
            // Thicken inside the existing 8x12 cell, retaining every original bit.
            // The rightmost column expands inward instead of spilling into the
            // next glyph. Paint only foreground, equally for black and white.
            const uint8_t ink = static_cast<uint8_t>(glyph[gy] | (glyph[gy] >> 1) |
                                                     ((glyph[gy] & 1) << 1));
            for (int gx = 0; gx < font::WIDTH; ++gx) {
                if (ink & (0x80 >> gx)) {
                    fillRect(cursor_x + gx * scale, cursor_y + gy * scale, scale, scale, black);
                }
            }
        }
        cursor_x += font::WIDTH * scale;
        if (cursor_x + font::WIDTH * scale > width()) {
            cursor_x = x;
            cursor_y += (font::HEIGHT + 2) * scale;
        }
        if (cursor_y >= height()) break;
    }
}

void EpaperDisplay::refresh() {
    (void)refresh(false);
}

bool EpaperDisplay::refresh(bool force_full) {
    if (!spi_) return false;
    if (!force_full && reference_valid_ && std::memcmp(previous_, framebuffer_, sizeof(previous_)) == 0) return true;
    const bool partial = !force_full && reference_valid_ && partial_count_ < partial_limit_;
    reference_valid_ = false; // Commit only after every transfer and activation completes.
    io_ok_ = true;
    if (needs_reset_) {
        gpio_set_level(board::EPD_PWR, 0); // Restore the active-LOW supply after sleep.
        vTaskDelay(pdMS_TO_TICKS(20));
        hardwareReset();
        needs_reset_ = false;
    }
    if (partial) {
        // The vendor partial reset preserves RAM. Never issue SWRESET here.
        hardwareReset();
        waitBusy();
        loadLut(waveform::PARTIAL);
        const uint8_t options[] = {0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0};
        sendCommand(0x37); sendBuffer(options, sizeof(options));
        sendCommand(0x3C); sendData(0x80);
        sendCommand(0x22); sendData(0xC0);
        sendCommand(0x20); waitBusy();
        // Retain the known-working framebuffer orientation after hardware reset.
        sendCommand(0x11); sendData(0x03);
        sendCommand(0x44); sendData(0x00); sendData(0x18);
        sendCommand(0x45); sendData(0x00); sendData(0x00); sendData(0xC7); sendData(0x00);
    } else {
        controllerInit(); // restore full border, addressing and controller settings
        loadLut(waveform::FULL);
    }
    sendCommand(0x4E);
    sendData(0x00);
    sendCommand(0x4F);
    sendData(0x00);
    sendData(0x00);

    if (partial) {
        // Explicitly seed old RAM on each update, independent of ping-pong copying.
        sendCommand(0x26);
        sendBuffer(previous_, sizeof(previous_));
        sendCommand(0x4E); sendData(0x00);
        sendCommand(0x4F); sendData(0x00); sendData(0x00);
    }
    sendCommand(0x24);
    sendBuffer(framebuffer_, sizeof(framebuffer_));
    if (!partial) {
        sendCommand(0x4E); sendData(0x00);
        sendCommand(0x4F); sendData(0x00); sendData(0x00);
        sendCommand(0x26);
        sendBuffer(framebuffer_, sizeof(framebuffer_));
    }

    sendCommand(0x22);
    sendData(partial ? 0xCF : 0xC7);
    sendCommand(0x20);
    if (!waitBusy(8000) || !io_ok_) {
        needs_reset_ = true;
        return false;
    }
    std::memcpy(previous_, framebuffer_, sizeof(previous_));
    reference_valid_ = true;
    partial_count_ = partial ? partial_count_ + 1 : 0;
    return true;
}

void EpaperDisplay::sleep() {
    reference_valid_ = false;
    needs_reset_ = true;
    partial_count_ = 0;
    sendCommand(0x10);
    sendData(0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(board::EPD_PWR, 1); // V2 supply disable is HIGH.
}

} // namespace display
