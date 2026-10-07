#pragma once
#include <string>
#include <cstdint>
#include <cstring>

namespace network { namespace live {
inline bool canDiscardShortWav(size_t bytes, bool saved_cleanly) {
    return saved_cleanly && bytes < 3200;
}
inline bool validPcmSize(size_t bytes) {
    return bytes > 0 && bytes <= 16000 * 2 * 30 && bytes % 2 == 0;
}
inline std::string endpoint(const std::string &id) {
    if (id.empty() || id.size() > 96) return {};
    for (unsigned char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return {};
    }
    return "/api/v1/live/" + id;
}
inline bool validResponse(const std::string &expected_id, const std::string &id,
                          const std::string &status, const std::string &model,
                          bool has_text, bool has_language) {
    return id == expected_id && status == "partial" && model == "whistle" &&
           has_text && has_language;
}

inline void appendText(std::string &text, const std::string &part, size_t limit) {
    if (part.empty() || text.size() >= limit) return;
    if (!text.empty()) text += ' ';
    const size_t remaining = limit - text.size();
    size_t n = part.size() < remaining ? part.size() : remaining;
    if (n < part.size()) {
        while (n > 0 && (static_cast<unsigned char>(part[n]) & 0xc0) == 0x80) --n;
    }
    text.append(part, 0, n);
}

// No allocation; storage is owned by the network session until both tasks join.
class PcmWindow {
public:
    PcmWindow(uint8_t *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}
    bool append(uint32_t sequence, const uint8_t *pcm, size_t bytes) {
        if (!buffer_ || !pcm || !validPcmSize(bytes) || bytes > capacity_) return false;
        if (sequence != expected_) used_ = 0;
        if (bytes > capacity_ - used_) return false;
        std::memcpy(buffer_ + used_, pcm, bytes);
        used_ += bytes;
        expected_ = sequence + 1;
        return true;
    }
    size_t size() const { return used_; }
    void reset() { used_ = 0; }
private:
    uint8_t *buffer_;
    size_t capacity_;
    size_t used_ = 0;
    uint32_t expected_ = 0;
};

class RefreshGate {
public:
    void reset(int64_t now_ms, const std::string &text, bool wifi) {
        last_ms_ = now_ms;
        text_ = text;
        wifi_ = wifi;
    }
    bool changed(int64_t now_ms, const std::string &text, bool wifi) {
        if (now_ms - last_ms_ < 2000 || (text == text_ && wifi == wifi_)) return false;
        reset(now_ms, text, wifi);
        return true;
    }
private:
    int64_t last_ms_ = 0;
    std::string text_;
    bool wifi_ = false;
};
}} // namespace network::live
