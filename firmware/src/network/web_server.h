#pragma once
#include "network/web_policy.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace network {
struct WebStatus {
    web::State state = web::State::Idle;
    bool wifi = false, stopping = false, recovery = false, connecting = false;
    size_t pending = 0;
    char base_url[64]{}, ip[16]{}, note_id[96]{}, text[4097]{}, live_text[4097]{};
};
// HTTP owns only this bounded mailbox/snapshot; application objects stay main-task-owned.
class WebServer {
public:
    bool start();
    void publish(const WebStatus &status);
    bool take(web::Command &command, unsigned &id);
    void complete(unsigned id, const char *result);
    bool takeConfig(char (&url)[64]);
private:
    static esp_err_t asset(httpd_req_t *request);
    static esp_err_t status(httpd_req_t *request);
    static esp_err_t command(httpd_req_t *request);
    static esp_err_t serverConfig(httpd_req_t *request);
    bool authorize(httpd_req_t *request);
    httpd_handle_t server_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    WebStatus snapshot_{};
    web::Mailbox mailbox_;
    unsigned command_id_ = 0;
    char command_result_[16]{};
    char config_url_[64]{};
};
} // namespace network
