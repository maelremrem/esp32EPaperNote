#include "network/web_server.h"
#include "network/server_url.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include "cJSON.h"
#include "secrets.h"

#define WEB_ASSET(name, file) \
    extern const unsigned char name##_begin[] asm("_binary_web_" file "_start"); \
    extern const unsigned char name##_end[] asm("_binary_web_" file "_end");
WEB_ASSET(html, "index_html")
WEB_ASSET(css, "style_css")
WEB_ASSET(tokens, "tokens_css")
WEB_ASSET(script, "app_js")
#undef WEB_ASSET

namespace network {
namespace {
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
private:
    SemaphoreHandle_t mutex_;
};
void responseHeaders(httpd_req_t *request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}
esp_err_t reply(httpd_req_t *request, const char *code, const char *json) {
    responseHeaders(request);
    httpd_resp_set_status(request, code);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}
std::string requestHeader(httpd_req_t *request, const char *name) {
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0) return {};
    if (length > 255) return "invalid";
    char value[256]{};
    return httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) == ESP_OK ? value : "invalid";
}
const char *stateText(web::State state) {
    switch (state) {
        case web::State::Idle: return "idle";
        case web::State::Menu: return "menu";
        case web::State::Recording: return "recording";
        case web::State::Syncing: return "syncing";
    }
    return "unknown";
}
}

bool WebServer::start() {
    if (server_) return true;
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return false;
    httpd_config_t options = HTTPD_DEFAULT_CONFIG();
    options.stack_size = 6144;
    options.max_uri_handlers = 10;
    options.max_open_sockets = 3;
    options.lru_purge_enable = true;
    options.recv_wait_timeout = options.send_wait_timeout = 3;
    if (httpd_start(&server_, &options) != ESP_OK) { server_ = nullptr; return false; }
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, asset, this}, {"/tokens.css", HTTP_GET, asset, this},
        {"/style.css", HTTP_GET, asset, this}, {"/app.js", HTTP_GET, asset, this},
        {"/api/status", HTTP_GET, status, this},
        {"/api/config/server", HTTP_GET, serverConfig, this},
        {"/api/config/server", HTTP_POST, serverConfig, this},
        {"/api/command/start", HTTP_POST, command, this},
        {"/api/command/stop", HTTP_POST, command, this},
        {"/api/command/sync", HTTP_POST, command, this}
    };
    for (const auto &route : routes) {
        if (httpd_register_uri_handler(server_, &route) == ESP_OK) continue;
        httpd_stop(server_); server_ = nullptr; return false;
    }
    return true;
}
void WebServer::publish(const WebStatus &status) {
    if (!mutex_) return;
    Lock lock(mutex_);
    snapshot_ = status;
}
bool WebServer::take(web::Command &command, unsigned &id) {
    if (!mutex_) return false;
    Lock lock(mutex_);
    return mailbox_.take(command, id);
}
bool WebServer::takeConfig(char (&url)[64]) {
    if (!mutex_) return false;
    Lock lock(mutex_);
    std::memcpy(url, config_url_, sizeof(url));
    return mailbox_.busy();
}
void WebServer::complete(unsigned id, const char *result) {
    if (!mutex_) return;
    Lock lock(mutex_);
    if (id != command_id_) return;
    web::copyText(command_result_, result);
    mailbox_.complete();
}
bool WebServer::authorize(httpd_req_t *request) {
    char ip[16];
    { Lock lock(mutex_); std::memcpy(ip, snapshot_.ip, sizeof(ip)); }
    if (!web::sameDevice(requestHeader(request, "Host"), requestHeader(request, "Origin"), ip)) {
        reply(request, "403 Forbidden", "{\"error\":\"origin_or_host\"}"); return false;
    }
    if (!web::authenticated(requestHeader(request, "Authorization"), VOICE_NOTES_API_TOKEN)) {
        reply(request, "401 Unauthorized", "{\"error\":\"unauthorized\"}"); return false;
    }
    return true;
}
esp_err_t WebServer::asset(httpd_req_t *request) {
    const unsigned char *begin = html_begin, *end = html_end;
    const char *type = "text/html; charset=utf-8";
    if (!std::strcmp(request->uri, "/style.css")) { begin = css_begin; end = css_end; type = "text/css; charset=utf-8"; }
    else if (!std::strcmp(request->uri, "/tokens.css")) { begin = tokens_begin; end = tokens_end; type = "text/css; charset=utf-8"; }
    else if (!std::strcmp(request->uri, "/app.js")) { begin = script_begin; end = script_end; type = "text/javascript; charset=utf-8"; }
    responseHeaders(request);
    httpd_resp_set_type(request, type);
    return httpd_resp_send(request, reinterpret_cast<const char *>(begin), end - begin);
}
esp_err_t WebServer::status(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    // The bounded transcript snapshot belongs on the heap, not a 6 KiB task stack.
    std::unique_ptr<WebStatus> snapshot(new (std::nothrow) WebStatus);
    if (!snapshot) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    unsigned id; bool busy; char result[16];
    {
        Lock lock(self.mutex_);
        *snapshot = self.snapshot_;
        id = self.command_id_; busy = self.mailbox_.busy();
        std::memcpy(result, self.command_result_, sizeof(result));
    }
    cJSON *json = cJSON_CreateObject();
    if (!json) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    const auto &s = *snapshot;
    const bool complete =
        cJSON_AddStringToObject(json, "state", stateText(s.state)) &&
        cJSON_AddBoolToObject(json, "wifi", s.wifi) &&
        cJSON_AddStringToObject(json, "ip", s.ip) &&
        cJSON_AddNumberToObject(json, "pending", s.pending) &&
        cJSON_AddBoolToObject(json, "stopping", s.stopping) &&
        cJSON_AddBoolToObject(json, "recovery", s.recovery) &&
        cJSON_AddBoolToObject(json, "connecting", s.connecting) &&
        cJSON_AddStringToObject(json, "note_id", s.note_id) &&
        cJSON_AddStringToObject(json, "text", s.text) &&
        cJSON_AddStringToObject(json, "live_text", s.live_text) &&
        cJSON_AddNumberToObject(json, "command_id", id) &&
        cJSON_AddStringToObject(json, "command_result", result) &&
        cJSON_AddBoolToObject(json, "command_busy", busy);
    char *body = complete ? cJSON_PrintUnformatted(json) : nullptr;
    cJSON_Delete(json);
    if (!body) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
    const esp_err_t sent = reply(request, "200 OK", body);
    cJSON_free(body);
    return sent;
}
esp_err_t WebServer::serverConfig(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    // URI handler user context is the same for GET/POST; method distinguishes them.
    if (request->method == HTTP_GET) {
        char url[64], result[16]; unsigned id; bool busy;
        { Lock lock(self.mutex_);
          std::memcpy(url, self.snapshot_.base_url, sizeof(url));
          std::memcpy(result, self.command_result_, sizeof(result));
          id = self.command_id_; busy = self.mailbox_.busy(); }
        cJSON *json = cJSON_CreateObject();
        if (!json) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
        const bool ok = cJSON_AddStringToObject(json, "base_url", url) &&
            cJSON_AddNumberToObject(json, "command_id", id) &&
            cJSON_AddStringToObject(json, "command_result", result) &&
            cJSON_AddBoolToObject(json, "command_busy", busy);
        char *body = ok ? cJSON_PrintUnformatted(json) : nullptr; cJSON_Delete(json);
        if (!body) return reply(request, "503 Service Unavailable", "{\"error\":\"memory\"}");
        const auto sent = reply(request, "200 OK", body); cJSON_free(body); return sent;
    }
    if (request->content_len > 256) return reply(request, "413 Payload Too Large", "{\"error\":\"oversize\"}");
    if (!request->content_len || requestHeader(request, "Content-Type") != "application/json")
        return reply(request, "400 Bad Request", "{\"error\":\"json_required\"}");
    char body[257]{}; size_t received = 0;
    while (received < request->content_len) {
        const int n = httpd_req_recv(request, body + received, request->content_len - received);
        if (n <= 0) return reply(request, "400 Bad Request", "{\"error\":\"incomplete_body\"}");
        received += static_cast<size_t>(n);
    }
    // URL and key need no escapes. Reject escaped NUL and controls before cJSON's C strings.
    for (size_t i = 0; i < received; ++i) if (body[i] == '\\' || static_cast<unsigned char>(body[i]) < 32)
        return reply(request, "400 Bad Request", "{\"error\":\"invalid_url\"}");
    cJSON *json = cJSON_ParseWithLengthOpts(body, received + 1, nullptr, true);
    cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "base_url") : nullptr;
    char ip[16]; { Lock lock(self.mutex_); std::memcpy(ip, self.snapshot_.ip, sizeof(ip)); }
    const bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 1 &&
        cJSON_IsString(item) && item->valuestring && validServerUrl(item->valuestring, ip);
    char url[64]{}; if (valid) web::copyText(url, item->valuestring); cJSON_Delete(json);
    if (!valid) return reply(request, "400 Bad Request", "{\"error\":\"invalid_url\"}");
    unsigned id = 0;
    { Lock lock(self.mutex_); const auto &s = self.snapshot_;
      if (web::allowed(web::Command::Configure, s.state, s.stopping, s.recovery, s.wifi, s.pending, s.connecting))
          id = self.mailbox_.submit(web::Command::Configure);
      if (id) { std::memcpy(self.config_url_, url, sizeof(url)); self.command_id_ = id; web::copyText(self.command_result_, "pending"); } }
    if (!id) return reply(request, "409 Conflict", "{\"error\":\"busy_or_unavailable\"}");
    char response[64]; std::snprintf(response, sizeof(response), "{\"id\":%u,\"status\":\"queued\"}", id);
    return reply(request, "202 Accepted", response);
}
esp_err_t WebServer::command(httpd_req_t *request) {
    auto &self = *static_cast<WebServer *>(request->user_ctx);
    if (!self.authorize(request)) return ESP_OK;
    if (request->content_len != 0) return reply(request, "413 Payload Too Large", "{\"error\":\"body_not_allowed\"}");
    web::Command command = web::Command::Start;
    if (!std::strcmp(request->uri, "/api/command/stop")) command = web::Command::Stop;
    else if (!std::strcmp(request->uri, "/api/command/sync")) command = web::Command::Sync;
    unsigned id = 0;
    {
        Lock lock(self.mutex_);
        const auto &s = self.snapshot_;
        if (web::allowed(command, s.state, s.stopping, s.recovery, s.wifi, s.pending, s.connecting)) id = self.mailbox_.submit(command);
        if (id) { self.command_id_ = id; web::copyText(self.command_result_, "pending"); }
    }
    if (!id) return reply(request, "409 Conflict", "{\"error\":\"busy_or_unavailable\"}");
    char json[64]; std::snprintf(json, sizeof(json), "{\"id\":%u,\"status\":\"queued\"}", id);
    return reply(request, "202 Accepted", json);
}
} // namespace network
