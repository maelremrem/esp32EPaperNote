#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "driver/spi_master.h"

namespace display {

class EpaperDisplay {
public:
    bool init();
    void clear(bool white = true);
    void drawPixel(int x, int y, bool black = true);
    void drawHLine(int x, int y, int w, bool black = true);
    void drawVLine(int x, int y, int h, bool black = true);
    void drawRect(int x, int y, int w, int h, bool black = true);
    void fillRect(int x, int y, int w, int h, bool black = true);
    void drawText(int x, int y, const std::string &text, int scale = 1, bool black = true);
    void refresh();
    void sleep();

    static constexpr int width() { return 200; }
    static constexpr int height() { return 200; }

private:
    void sendCommand(uint8_t value);
    void sendData(uint8_t value);
    void sendBuffer(const uint8_t *data, size_t len);
    bool waitBusy(uint32_t timeout_ms = 5000);
    void hardwareReset();
    void controllerInit();
    char transliterateUtf8(const char *&p) const;

    spi_device_handle_t spi_ = nullptr;
    uint8_t framebuffer_[200 * 200 / 8]{};
};

} // namespace display
