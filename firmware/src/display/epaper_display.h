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
    void refresh(); // automatic: first full, then changed-frame partial updates
    bool refresh(bool force_full); // false selects automatic; true forces a clean full update
    void sleep();
    // 0 = full only; preserve accumulated partials when changing the budget.
    void setPartialRefreshLimit(uint8_t limit) { partial_limit_ = limit <= 100 ? limit : 10; }
    uint8_t partialRefreshLimit() const { return partial_limit_; }

    static constexpr int width() { return 200; }
    static constexpr int height() { return 200; }

private:
    void sendCommand(uint8_t value);
    void sendData(uint8_t value);
    void sendBuffer(const uint8_t *data, size_t len);
    bool waitBusy(uint32_t timeout_ms = 5000);
    void hardwareReset();
    void controllerInit();
    void loadLut(const uint8_t *lut);
    char transliterateUtf8(const char *&p) const;

    spi_device_handle_t spi_ = nullptr;
    uint8_t framebuffer_[200 * 200 / 8]{};
    uint8_t previous_[200 * 200 / 8]{};
    bool reference_valid_ = false;
    uint8_t partial_count_ = 0;
    uint8_t partial_limit_ = 10;
    bool io_ok_ = true;
    bool needs_reset_ = false;
};

} // namespace display
