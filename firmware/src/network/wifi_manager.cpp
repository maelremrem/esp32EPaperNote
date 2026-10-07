#include "network/wifi_manager.h"

#include <cstring>
#include <cstdio>

#include "project_config.h"
#include "secrets.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

namespace network {

static const char *TAG = "wifi";
static EventGroupHandle_t s_events = nullptr;
static constexpr EventBits_t GOT_IP = BIT0;

static bool s_sntp_started = false;

static void eventHandler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_events) {
            xEventGroupClearBits(s_events, GOT_IP);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_events) {
            xEventGroupSetBits(s_events, GOT_IP);
        }
    }
}

bool WifiManager::init() {
    if (initialized_) {
        return true;
    }

    if (esp_netif_init() != ESP_OK) return false;
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return false;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr) != ESP_OK) return false;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, nullptr) != ESP_OK) return false;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;

    s_events = xEventGroupCreate();
    initialized_ = s_events != nullptr;
    return initialized_;
}

bool WifiManager::connectOne(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!ssid || !ssid[0]) {
        return false;
    }

    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(cfg.sta.password), password ? password : "", sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, GOT_IP);
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    ESP_LOGI(TAG, "Connecting to %s", ssid);
    if (esp_wifi_connect() != ESP_OK) return false;

    // Intentional disconnect events can arrive after switching SSIDs. They must
    // not terminate the new attempt: only GOT_IP or its timeout is decisive.
    const EventBits_t bits = xEventGroupWaitBits(
        s_events,
        GOT_IP,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );
    if (bits & GOT_IP) {
        startSntp();
        return true;
    }
    return false;
}

bool WifiManager::connectPreferred(WifiProgressCallback progress) {
    const char *ssids[] = {WIFI_HOME_SSID, WIFI_IPHONE_SSID};
    const char *passwords[] = {WIFI_HOME_PASSWORD, WIFI_IPHONE_PASSWORD};
    const auto report = [&](size_t index, WifiAttemptStatus status, const std::string &ip = {}) {
        if (progress) progress(index, status, ip);
    };
    const bool ready = initialized_ || init();
    bool success = false;
    for (size_t i = 0; i < 2; ++i) {
        if (!ssids[i] || !ssids[i][0]) {
            report(i, WifiAttemptStatus::Disabled);
        } else if (success) {
            report(i, WifiAttemptStatus::Skipped);
        } else if (!ready) {
            report(i, WifiAttemptStatus::Failed);
        } else {
            report(i, WifiAttemptStatus::Connecting);
            success = connectOne(ssids[i], passwords[i], config::WIFI_CONNECT_TIMEOUT_MS);
            report(i, success ? WifiAttemptStatus::Connected : WifiAttemptStatus::Failed,
                success ? ipAddress() : std::string{});
        }
    }
    if (success) return true;

    ESP_LOGW(TAG, "No configured Wi-Fi available");
    return false;
}

bool WifiManager::connected() const {
    if (!initialized_ || !(xEventGroupGetBits(s_events) & GOT_IP)) {
        return false;
    }
    wifi_ap_record_t ap{};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

std::string WifiManager::currentSsid() const {
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return {};
    }
    return reinterpret_cast<const char *>(ap.ssid);
}

std::string WifiManager::ipAddress() const {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return {};
    }
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) {
        return {};
    }
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), IPSTR, IP2STR(&info.ip));
    return buffer;
}

void WifiManager::startSntp() {
    if (s_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
}

} // namespace network
