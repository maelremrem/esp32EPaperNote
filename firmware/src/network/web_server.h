#pragma once
#include "network/web_policy.h"
#include <cstdio>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace network {
struct WebNote { char id[97]{}; bool transcribed=false, audio=false; };
struct WebStatus {
    web::State state = web::State::Idle;
    bool wifi = false, stopping = false, recovery = false, connecting = false;
    size_t pending = 0;
    size_t notes_count=0; WebNote notes[40]{};
    bool mounted=false, audio_ready=false;
    bool usage_known=false;
    bool token_configured=false;
    unsigned server_revision=0;
    uint64_t total_bytes=0, free_bytes=0;
    unsigned partial_limit=10, format_challenge=0;
    char ssid[2][33]{}, current_ssid[33]{}, storage_error[256]{};
    char base_url[64]{}, ip[16]{}, note_id[96]{}, text[4097]{}, live_text[4097]{};
};
// HTTP owns only this bounded mailbox/snapshot; application objects stay main-task-owned.
class WebServer {
public:
    bool start();
    void stop();
    // Main-owned six-digit PIN, absolute ten-minute TTL, five failed guesses.
    std::string openLocalSession();
    void revokeLocalSession();
    bool downloadBusy();
    bool takeDownload(char (&id)[97], bool &markdown);
    // Transfers FILE ownership to handler; stale request closes immediately on main.
    void provideDownload(unsigned lease, FILE *file, uint64_t bytes);
    void publish(const WebStatus &status);
    bool take(web::Command &command, unsigned &id);
    void complete(unsigned id, const char *result);
    bool takeConfig(char (&url)[64]);
    bool takeSettings(web::SettingsWrite &settings);
private:
    static esp_err_t asset(httpd_req_t *request);
    static esp_err_t status(httpd_req_t *request);
    static esp_err_t command(httpd_req_t *request);
    static esp_err_t serverConfig(httpd_req_t *request);
    static esp_err_t settings(httpd_req_t *request);
    static esp_err_t notes(httpd_req_t *request);
    static esp_err_t download(httpd_req_t *request);
    static esp_err_t closeSession(httpd_req_t *request);
    bool authorize(httpd_req_t *request);
    httpd_handle_t server_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    WebStatus snapshot_{};
    char session_code_[7]{};
    unsigned failed_pin_attempts_=0;
    int64_t session_deadline_=0;
    web::Mailbox mailbox_;
    bool download_active_=false, download_ready_=false, download_markdown_=false;
    unsigned download_lease_=0;
    FILE *download_file_=nullptr;
    uint64_t download_bytes_=0;
    char download_id_[97]{};
    unsigned command_id_ = 0;
    char command_result_[16]{};
    char config_url_[64]{};
    web::SettingsWrite settings_{};
};
} // namespace network
