#include "display/wifi_qr.h"
#include "display/qr/qrcodegen.h"
#include <cstring>

namespace display::qr {
namespace {
constexpr int maxVersion = 5; // (37 modules + 2*4 quiet modules)*2 = 90px.
constexpr int scale = 2;
constexpr int quiet = 4;
constexpr size_t payloadCapacity = 2 * (32 + 63) + 32;

bool valid(const std::string &value) {
    for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool append(char *out, size_t &used, const std::string &value, bool escape) {
    for (char c : value) {
        const bool special = escape && (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"');
        if (used + (special ? 2 : 1) >= payloadCapacity) return false;
        if (special) out[used++] = '\\';
        out[used++] = c;
    }
    out[used] = '\0';
    return true;
}
} // namespace

bool drawWifi(EpaperDisplay &display, const std::string &ssid, const std::string &password) {
    if (ssid.empty() || ssid.size() > 32 || password.size() < 8 || password.size() > 63 ||
        !valid(ssid) || !valid(password)) return false;
    char payload[payloadCapacity]{};
    size_t used = 0;
    if (!append(payload, used, "WIFI:T:WPA;S:", false) || !append(payload, used, ssid, true) ||
        !append(payload, used, ";P:", false) || !append(payload, used, password, true) ||
        !append(payload, used, ";;", false)) return false;
    uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion)];
    uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion)];
    // Low is the minimum ECC; boost when the same version has spare capacity.
    const bool encoded = qrcodegen_encodeText(payload, temporary, code, qrcodegen_Ecc_LOW,
        1, maxVersion, qrcodegen_Mask_AUTO, true);
    if (!encoded) return false;
    const int modules = qrcodegen_getSize(code);
    const int side = (modules + 2 * quiet) * scale;
    const int left = (display.width() - side) / 2;
    const int top = 28 + (90 - side) / 2;
    // Explicit white area also preserves quiet zone on an inverted framebuffer.
    display.fillRect(left, top, side, side, false);
    for (int y = 0; y < modules; ++y)
        for (int x = 0; x < modules; ++x)
            if (qrcodegen_getModule(code, x, y))
                display.fillRect(left + (quiet + x) * scale, top + (quiet + y) * scale,
                                 scale, scale, true);
    return true;
}
} // namespace display::qr
