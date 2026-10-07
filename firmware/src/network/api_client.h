#pragma once

#include <string>
#include <atomic>
#include <cstdint>

namespace network {

struct TranscriptResult {
    bool ok = false;
    int http_status = 0;
    std::string id;
    std::string status;
    std::string language;
    std::string text;
    std::string model;
    double duration = 0.0;
    std::string error;
};

class ApiClient {
public:
    static std::string defaultBaseUrl();
    static std::string baseUrl();
    // Main only, after recording workers have joined and outside synchronization.
    static void setBaseUrl(const std::string &url);
    TranscriptResult transcribe(const std::string &note_id, const std::string &wav_path) const;
    TranscriptResult preview(const std::string &note_id, const uint8_t *pcm, size_t bytes,
                             const std::atomic<bool> &cancelled) const;
};

} // namespace network
