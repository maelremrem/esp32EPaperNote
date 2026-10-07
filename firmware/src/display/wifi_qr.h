#pragma once

#include <string>
#include "display/epaper_display.h"

namespace display::qr {
// Fixed-size encoding workspace; no credential logging or dynamic QR buffers.
// Returns false for invalid credentials or a payload too large for the display.
bool drawWifi(EpaperDisplay &display, const std::string &ssid, const std::string &password);
} // namespace display::qr
