#pragma once

#include <string>

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
    TranscriptResult transcribe(const std::string &note_id, const std::string &wav_path) const;
};

} // namespace network
