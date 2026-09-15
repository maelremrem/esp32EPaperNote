#include "network/api_client.h"

#include <cstdio>
#include <sys/stat.h>

#include "project_config.h"
#include "secrets.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace network {

static const char *TAG = "api";

static std::string jsonString(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

TranscriptResult ApiClient::transcribe(const std::string &note_id, const std::string &wav_path) const {
    TranscriptResult result;
    result.id = note_id;

    struct stat st{};
    if (::stat(wav_path.c_str(), &st) != 0 || st.st_size <= 44) {
        result.error = "WAV file missing or empty";
        return result;
    }

    FILE *file = std::fopen(wav_path.c_str(), "rb");
    if (!file) {
        result.error = "Cannot open WAV file";
        return result;
    }

    std::string url = std::string(VOICE_NOTES_API_BASE_URL) + "/api/v1/notes/" + note_id + "/transcribe";
    const bool https = url.rfind("https://", 0) == 0;

    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.timeout_ms = config::HTTP_TIMEOUT_MS;
    cfg.keep_alive_enable = true;
    if (https) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        std::fclose(file);
        result.error = "HTTP client init failed";
        return result;
    }

    const std::string auth = std::string("Bearer ") + VOICE_NOTES_API_TOKEN;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Type", "waveshare-esp32-s3-epaper-1.54-v2");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(st.st_size));
    if (err != ESP_OK) {
        result.error = std::string("HTTP open failed: ") + esp_err_to_name(err);
        std::fclose(file);
        esp_http_client_cleanup(client);
        return result;
    }

    uint8_t buffer[4096];
    size_t sent_total = 0;
    while (!std::feof(file)) {
        const size_t n = std::fread(buffer, 1, sizeof(buffer), file);
        if (n == 0) {
            break;
        }
        const int written = esp_http_client_write(client, reinterpret_cast<const char *>(buffer), static_cast<int>(n));
        if (written < 0 || static_cast<size_t>(written) != n) {
            result.error = "HTTP upload interrupted";
            std::fclose(file);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return result;
        }
        sent_total += n;
    }
    std::fclose(file);
    ESP_LOGI(TAG, "Uploaded %u bytes for %s", static_cast<unsigned>(sent_total), note_id.c_str());

    esp_http_client_fetch_headers(client);
    result.http_status = esp_http_client_get_status_code(client);

    std::string body;
    body.reserve(1024);
    while (body.size() < config::HTTP_RESPONSE_MAX) {
        char chunk[512];
        const int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        body.append(chunk, static_cast<size_t>(n));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result.http_status != 200) {
        result.error = "HTTP " + std::to_string(result.http_status) + ": " + body;
        return result;
    }

    cJSON *root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!root) {
        result.error = "Invalid JSON response";
        return result;
    }

    result.id = jsonString(root, "id");
    result.status = jsonString(root, "status");
    result.language = jsonString(root, "language");
    result.text = jsonString(root, "text");
    result.model = jsonString(root, "model");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
    if (cJSON_IsNumber(duration)) {
        result.duration = duration->valuedouble;
    }
    result.ok = result.status == "done";
    if (!result.ok) {
        result.error = jsonString(root, "error");
    }
    cJSON_Delete(root);
    return result;
}

} // namespace network
