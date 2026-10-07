#include "network/api_client.h"

#include <cstdio>
#include <mutex>
#include <algorithm>
#include <sys/stat.h>

#include "project_config.h"
#include "secrets.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "network/live_preview_helpers.h"

namespace network {

namespace {
std::mutex base_mutex;
std::string runtime_base = VOICE_NOTES_API_BASE_URL;
std::string runtime_token = VOICE_NOTES_API_TOKEN;
struct Target { std::string url, token; };
Target targetSnapshot() { std::lock_guard<std::mutex> lock(base_mutex); return {runtime_base,runtime_token}; }
}
std::string ApiClient::defaultBaseUrl() { return VOICE_NOTES_API_BASE_URL; }
std::string ApiClient::baseUrl() {
    std::lock_guard<std::mutex> lock(base_mutex);
    return runtime_base; // immutable per-request copy; never retain a mutable c_str
}
void ApiClient::setBaseUrl(const std::string &url) {
    std::lock_guard<std::mutex> lock(base_mutex);
    runtime_base = url;
}

bool ApiClient::tokenConfigured() { std::lock_guard<std::mutex> lock(base_mutex); return !runtime_token.empty(); }
void ApiClient::setTarget(const std::string &url, const std::string &token, bool replace_token) {
    std::lock_guard<std::mutex> lock(base_mutex);
    runtime_base=url;
    if (replace_token) runtime_token=token;
}

static std::string jsonString(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

HealthStatus ApiClient::health() const {
    const auto target = targetSnapshot();
    if (target.url.empty() || target.token.empty()) return HealthStatus::Skipped;
    const std::string url = target.url + "/health";
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.timeout_ms = 5000;
    cfg.disable_auto_redirect = true;
    if (url.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&cfg);
    if (!client) return HealthStatus::Unavailable;
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    // /health is public: do not send the transcription credential.
    bool valid = esp_http_client_open(client, 0) == ESP_OK;
    std::string body;
    if (valid) {
        valid = esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200;
        const int64_t deadline = esp_timer_get_time() + 5000000;
        while (valid && body.size() < 1024 && !esp_http_client_is_complete_data_received(client)) {
            char chunk[256];
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n <= 0 || esp_timer_get_time() > deadline) { valid = false; break; }
            body.append(chunk, static_cast<size_t>(n));
        }
        valid = valid && body.size() < 1024 && esp_http_client_is_complete_data_received(client);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!valid) return HealthStatus::Unavailable;
    cJSON *root = cJSON_ParseWithLengthOpts(body.c_str(), body.size()+1, nullptr, true);
    const bool ready = root && jsonString(root, "status") == "ok" &&
        jsonString(root, "service") == "ESP32 Voice Notes STT";
    cJSON_Delete(root);
    // Whistle loads lazily: model_loaded=false is healthy, not a failure.
    return ready ? HealthStatus::Ready : HealthStatus::Unavailable;
}

TranscriptResult ApiClient::transcribe(const std::string &note_id, const std::string &wav_path,
                                        const std::atomic<bool> *cancelled) const {
    TranscriptResult result;
    result.id = note_id;
    const int64_t deadline = esp_timer_get_time() + config::HTTP_TIMEOUT_MS * 1000LL;
    const auto abort = [&]() { return (cancelled && cancelled->load()) || esp_timer_get_time() >= deadline; };
    const auto failure = [&](const char *message) {
        result.cancelled = cancelled && cancelled->load();
        result.ok = false;
        result.error = result.cancelled ? "Sync cancelled" : message;
        return result;
    };
    if (abort()) return failure("Request deadline exceeded");
    const auto target = targetSnapshot();
    if (target.token.empty()) return failure("Transcription token not configured");
    struct stat st{};
    if (::stat(wav_path.c_str(), &st) != 0 || st.st_size <= 44 || st.st_size > INT32_MAX)
        return failure("WAV file missing or empty");
    FILE *file = std::fopen(wav_path.c_str(), "rb");
    if (!file) return failure("Cannot open WAV file");
    const std::string url = target.url + "/api/v1/notes/" + note_id + "/transcribe";
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    // Cooperative cancellation: only this worker touches the HTTP handle. Short
    // socket waits are retried for slow inference, within the total deadline.
    cfg.timeout_ms = cancelled ? 2000 : config::HTTP_TIMEOUT_MS;
    cfg.disable_auto_redirect = true;
    if (url.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&cfg);
    if (!client) { std::fclose(file); return failure("HTTP client init failed"); }
    const std::string auth = "Bearer " + target.token;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Type", "waveshare-esp32-s3-epaper-1.54-v2");
    bool ok = !abort() && esp_http_client_open(client, static_cast<int>(st.st_size)) == ESP_OK;
    uint8_t buffer[4096];
    size_t total = 0;
    while (ok && !abort() && total < static_cast<size_t>(st.st_size)) {
        const size_t n = std::fread(buffer, 1, sizeof(buffer), file);
        if (!n) { ok = false; break; }
        size_t offset = 0;
        while (ok && !abort() && offset < n) {
            const int written = esp_http_client_write(client, reinterpret_cast<const char *>(buffer + offset), static_cast<int>(n - offset));
            if (written <= 0) { ok = false; break; }
            offset += static_cast<size_t>(written);
        }
        total += offset;
    }
    std::fclose(file);
    ok = ok && !abort() && total == static_cast<size_t>(st.st_size);
    if (ok) {
        int64_t headers;
        do { headers = esp_http_client_fetch_headers(client); }
        while (headers == -ESP_ERR_HTTP_EAGAIN && !abort());
        ok = headers >= 0 && !abort();
        result.http_status = esp_http_client_get_status_code(client);
        ok = ok && result.http_status == 200;
    }
    std::string body;
    body.reserve(1024);
    while (ok && !abort() && !esp_http_client_is_complete_data_received(client)) {
        if (body.size() >= config::HTTP_RESPONSE_MAX) { ok = false; break; }
        char chunk[512];
        const int n = esp_http_client_read(client, chunk, static_cast<int>(std::min(sizeof(chunk), config::HTTP_RESPONSE_MAX-body.size())));
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n <= 0) { ok = false; break; }
        body.append(chunk, static_cast<size_t>(n));
    }
    ok = ok && !abort() && esp_http_client_is_complete_data_received(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok) return failure("HTTP request failed or deadline exceeded");
    cJSON *root = cJSON_ParseWithLengthOpts(body.c_str(), body.size()+1, nullptr, true);
    if (!root) return failure("Invalid JSON response");
    result.id = jsonString(root, "id");
    result.status = jsonString(root, "status");
    result.language = jsonString(root, "language");
    result.text = jsonString(root, "text");
    result.model = jsonString(root, "model");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    if (cJSON_IsNumber(duration)) result.duration = duration->valuedouble;
    result.ok = result.status == "done" && result.id == note_id;
    cJSON_Delete(root);
    if (abort()) return failure("Request deadline exceeded");
    if (!result.ok) result.error = "Unexpected transcription response";
    return result;
}

TranscriptResult ApiClient::preview(const std::string &note_id, const uint8_t *pcm,
                                    size_t bytes, const std::atomic<bool> &cancelled) const {
    TranscriptResult result;
    const std::string path = live::endpoint(note_id);
    if (path.empty() || !pcm || !live::validPcmSize(bytes) || cancelled.load()) {
        result.error = "Invalid or cancelled live request";
        return result;
    }
    const auto target=targetSnapshot();
    if (target.token.empty()) { result.error="Transcription token not configured"; return result; }
    const std::string url = target.url + path;
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.timeout_ms = config::LIVE_HTTP_TIMEOUT_MS;
    cfg.disable_auto_redirect = true;
    if (url.rfind("https://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto client = esp_http_client_init(&cfg);
    if (!client) { result.error = "Live HTTP init failed"; return result; }
    const int64_t deadline = esp_timer_get_time() + config::LIVE_REQUEST_DEADLINE_MS * 1000LL;
    const auto abort = [&]() { return cancelled.load() || esp_timer_get_time() >= deadline; };
    const std::string auth = std::string("Bearer ") + target.token;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    bool ok = !abort() && esp_http_client_open(client, static_cast<int>(bytes)) == ESP_OK;
    size_t sent = 0;
    while (ok && sent < bytes) {
        if (abort()) { ok = false; break; }
        const int n = esp_http_client_write(client, reinterpret_cast<const char *>(pcm + sent),
                                           static_cast<int>(std::min<size_t>(4096, bytes - sent)));
        if (n <= 0) { ok = false; break; }
        sent += static_cast<size_t>(n); // ESP-IDF can write fewer bytes than requested.
    }
    if (ok) ok = !abort() && esp_http_client_fetch_headers(client) >= 0;
    result.http_status = esp_http_client_get_status_code(client);
    std::string body;
    body.reserve(1024);
    while (ok && !esp_http_client_is_complete_data_received(client)) {
        if (abort() || body.size() >= config::HTTP_RESPONSE_MAX) { ok = false; break; }
        char chunk[512];
        const int n = esp_http_client_read(client, chunk, static_cast<int>(
            std::min<size_t>(sizeof(chunk), config::HTTP_RESPONSE_MAX - body.size())));
        if (n <= 0) { ok = false; break; }
        body.append(chunk, static_cast<size_t>(n));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok || abort() || result.http_status != 200) {
        result.error = "Live request failed, cancelled, or oversized";
        return result;
    }
    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root) { result.error = "Invalid live JSON"; return result; }
    result.id = jsonString(root, "id");
    result.status = jsonString(root, "status");
    result.text = jsonString(root, "text");
    result.language = jsonString(root, "language");
    result.model = jsonString(root, "model");
    result.ok = live::validResponse(note_id, result.id, result.status, result.model,
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "text")),
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "language")));
    if (!result.ok) result.error = "Unexpected live response contract";
    cJSON_Delete(root);
    return result;
}

} // namespace network
