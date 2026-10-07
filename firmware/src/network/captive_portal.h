#pragma once
#include "network/wifi_manager.h"
#include "esp_http_server.h"
#include <atomic>
#include <string>

namespace network {
// In-place bounded DNS A response; zero rejects unsupported/malformed queries.
size_t captiveDnsReply(uint8_t *packet, size_t length, size_t capacity);

class CaptivePortal {
public:
    ~CaptivePortal();
    bool init();
    bool start(WifiManager &wifi);
    bool stop();
    bool active() const { return active_.load(); }
    const std::string &ssid() const { return ssid_; }
    const std::string &password() const { return password_; }
    const std::string &address() const { return address_; }
    bool expired() const;
    bool takeProfiles(WifiProfiles &profiles);
    // Call from the main loop while physically active; HTTP only queues/snapshots.
    void pollScan();
    void complete(bool success);
private:
    static void dnsLoop(void *);
    static esp_err_t index(httpd_req_t *request);
    static esp_err_t save(httpd_req_t *request);
    static esp_err_t scan(httpd_req_t *request);
    static esp_err_t scanResults(httpd_req_t *request);
    bool acceptRequest(httpd_req_t *request, bool write) const;
    std::atomic<bool> active_{false};
    WifiManager *wifi_ = nullptr;
    httpd_handle_t server_ = nullptr;
    void *dns_task_ = nullptr;
    void *dns_done_ = nullptr;
    int dns_socket_ = -1;
    void *mutex_ = nullptr;
    WifiProfiles pending_{};
    bool pending_ready_ = false;
    bool pending_reserved_ = false;
    bool result_ = false;
    WifiScanState scan_state_ = WifiScanState::Idle;
    WifiScanResults scan_results_;
    std::string scan_error_;
    int64_t scan_queued_at_ = 0, scan_completed_at_ = 0;
    std::string nonce_;
    bool cleanup_failed_ = false;
    int64_t expires_at_ = 0;
    std::string ssid_, password_, token_, address_ = "192.168.4.1";
};
} // namespace network
