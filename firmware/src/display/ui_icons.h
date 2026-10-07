#pragma once

#include <cstdint>
#include "display/epaper_display.h"

namespace display::icons {

// Hand-drawn 16px monochrome symbols; scale by whole pixels on the 200px panel.
enum class Icon { Note, Mic, Wifi, Sync, Card, Gear, Info, Back, Clock, Warning };
inline constexpr uint16_t pixels[][16] = {
    {0x3fc0,0x2040,0x2070,0x2010,0x2010,0x2f90,0x2010,0x2f90,0x2010,0x2e10,0x2010,0x2010,0x2010,0x3ff0,0,0},
    {0x03c0,0x07e0,0x07e0,0x07e0,0x07e0,0x07e0,0x27e4,0x27e4,0x23c4,0x2004,0x1008,0x0ff0,0x0180,0x0180,0x07e0,0},
    {0,0x0ff0,0x3ffc,0x700e,0x6006,0x07e0,0x0ff0,0x1818,0,0x03c0,0x0660,0,0x0180,0x03c0,0x0180,0},
    {0,0x07c2,0x1fe6,0x381e,0x300e,0x600e,0x6000,0,0,0x0006,0x7006,0x700c,0x781c,0x67f8,0x43e0,0},
    {0x0ff8,0x1aa8,0x2aa8,0x4aa8,0x4aa8,0x4008,0x4008,0x4008,0x4008,0x4008,0x4008,0x4008,0x4008,0x7ff8,0,0},
    {0x0180,0x03c0,0x1bd8,0x3ffc,0x1c38,0x1818,0x73ce,0x77ee,0x77ee,0x73ce,0x1818,0x1c38,0x3ffc,0x1bd8,0x03c0,0x0180},
    {0x07e0,0x1818,0x2004,0x4182,0x4182,0x4002,0x4382,0x4182,0x4182,0x4182,0x43c2,0x2004,0x1818,0x07e0,0,0},
    {0,0,0x0200,0x0600,0x0c00,0x1800,0x3ff8,0x7ff8,0x3ff8,0x1800,0x0c00,0x0600,0x0200,0,0,0},
    {0x07e0,0x1818,0x2184,0x4182,0x4182,0x4182,0x41f2,0x4002,0x4002,0x4002,0x4002,0x2004,0x1818,0x07e0,0,0},
    {0x0180,0x03c0,0x0660,0x0660,0x0c30,0x0db0,0x1998,0x1998,0x319c,0x300c,0x6186,0x6186,0xc003,0xffff,0,0},
};

inline void draw(EpaperDisplay &display, Icon icon, int x, int y, int scale = 1, bool black = true) {
    if (scale <= 0) return;
    if (icon == Icon::Warning) {
        // Rasterize at the final resolution: magnifying a 16px bitmap makes
        // 4px stair steps on the large SD warning. The panel remains 1-bit.
        const int size = 16 * scale;
        const int top = scale;
        const int bottom = 14 * scale - 1;
        const int span = (size - 2) / 2 - scale;
        const int stroke = scale;
        for (int row = top; row <= bottom; ++row) {
            const int half = span * (row - top) / (bottom - top);
            const int left = (size - 1) / 2 - half;
            const int right = size - 1 - left;
            for (int col = left; col <= right; ++col) {
                const bool outline = col < left + stroke || col > right - stroke ||
                                     row > bottom - stroke;
                const bool centre = col >= size / 2 - scale && col < size / 2 + scale;
                const bool mark = centre && ((row >= 6 * scale && row < 10 * scale) ||
                                              (row >= 11 * scale && row < 12 * scale));
                if (outline || mark) display.drawPixel(x + col, y + row, black);
            }
        }
        return;
    }
    const auto &rows = pixels[static_cast<unsigned>(icon)];
    for (int row = 0; row < 16; ++row)
        for (int col = 0; col < 16; ++col)
            if (rows[row] & (0x8000 >> col))
                display.fillRect(x + col * scale, y + row * scale, scale, scale, black);
}

inline void disc(EpaperDisplay &display, int cx, int cy, int radius, bool black = true) {
    for (int y = -radius; y <= radius; ++y)
        for (int x = -radius; x <= radius; ++x)
            if (x * x + y * y <= radius * radius) display.drawPixel(cx + x, cy + y, black);
}

} // namespace display::icons
