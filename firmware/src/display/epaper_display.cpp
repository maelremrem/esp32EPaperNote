#include "display/epaper_display.h"

#include <algorithm>
#include <cstring>

#include "board_pins.h"
#include "display/font8x12.h"
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

    gpio_set_level(board::EPD_PWR, 1);
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
    return true;
}

void EpaperDisplay::hardwareReset() {
    gpio_set_level(board::EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(board::EPD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(board::EPD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

bool EpaperDisplay::waitBusy(uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    // Waveshare V2 examples use HIGH = busy, LOW = idle.
    while (gpio_get_level(board::EPD_BUSY) == 1) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "BUSY timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

void EpaperDisplay::controllerInit() {
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
    sendData(0x05);

    sendCommand(0x18); // use internal temperature sensor
    sendData(0x80);

    sendCommand(0x4E);
    sendData(0x00);
    sendCommand(0x4F);
    sendData(0x00);
    sendData(0x00);
    waitBusy();
}

void EpaperDisplay::sendCommand(uint8_t value) {
    gpio_set_level(board::EPD_DC, 0);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &value;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(board::EPD_CS, 1);
}

void EpaperDisplay::sendData(uint8_t value) {
    gpio_set_level(board::EPD_DC, 1);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = 8;
    t.tx_buffer = &value;
    spi_device_polling_transmit(spi_, &t);
    gpio_set_level(board::EPD_CS, 1);
}

void EpaperDisplay::sendBuffer(const uint8_t *data, size_t len) {
    gpio_set_level(board::EPD_DC, 1);
    gpio_set_level(board::EPD_CS, 0);
    spi_transaction_t t{};
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(spi_, &t);
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
    const auto c0 = static_cast<unsigned char>(*p);
    if (c0 < 0x80) {
        return *p++;
    }
    if (c0 == 0xC3 && p[1]) {
        const auto c1 = static_cast<unsigned char>(p[1]);
        p += 2;
        switch (c1) {
            case 0xA0: case 0xA2: case 0xA4: return 'a';
            case 0xA7: return 'c';
            case 0xA8: case 0xA9: case 0xAA: case 0xAB: return 'e';
            case 0xAE: case 0xAF: return 'i';
            case 0xB4: case 0xB6: return 'o';
            case 0xB9: case 0xBB: case 0xBC: return 'u';
            case 0x80: case 0x82: case 0x84: return 'A';
            case 0x87: return 'C';
            case 0x88: case 0x89: case 0x8A: case 0x8B: return 'E';
            default: return '?';
        }
    }
    ++p;
    return '?';
}

void EpaperDisplay::drawText(int x, int y, const std::string &text, int scale, bool black) {
    const char *p = text.c_str();
    int cursor_x = x;
    int cursor_y = y;
    while (*p) {
        char c = transliterateUtf8(p);
        if (c == '\n') {
            cursor_x = x;
            cursor_y += (font::HEIGHT + 2) * scale;
            continue;
        }
        if (c < 32 || c > 126) c = '?';
        const auto &glyph = font::GLYPHS[static_cast<unsigned char>(c) - 32];
        for (int gy = 0; gy < font::HEIGHT; ++gy) {
            for (int gx = 0; gx < font::WIDTH; ++gx) {
                if (glyph[gy] & (0x80 >> gx)) {
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
    sendCommand(0x4E);
    sendData(0x00);
    sendCommand(0x4F);
    sendData(0x00);
    sendData(0x00);

    sendCommand(0x24);
    sendBuffer(framebuffer_, sizeof(framebuffer_));

    sendCommand(0x22);
    sendData(0xF7); // full update using controller waveform/temperature settings
    sendCommand(0x20);
    waitBusy(8000);
}

void EpaperDisplay::sleep() {
    sendCommand(0x10);
    sendData(0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(board::EPD_PWR, 0);
}

} // namespace display
