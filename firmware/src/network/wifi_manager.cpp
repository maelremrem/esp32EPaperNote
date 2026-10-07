#include "network/wifi_manager.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include "esp_timer.h"

#include "project_config.h"
#include "secrets.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs.h"

namespace network {

static const char *TAG = "wifi";
static EventGroupHandle_t s_events = nullptr;
static constexpr EventBits_t GOT_IP = BIT0;

static bool s_sntp_started = false;
static std::atomic<bool> s_portal_radio{false}, s_scan_armed{false}, s_scan_done{false};
static std::atomic<unsigned> s_scan_status{0}, s_scan_generation{0};
static std::atomic<bool> s_scan_radio_ready{false};
#ifdef ESP_PLATFORM
ESP_EVENT_DEFINE_BASE(PORTAL_SCAN_BARRIER);
static bool scanBarrier() {
    s_scan_radio_ready = false;
    const unsigned generation=++s_scan_generation;
    // FIFO event-loop fence after synchronous radio stop, including queued old SCAN_DONE.
    return esp_event_post(PORTAL_SCAN_BARRIER,0,&generation,sizeof generation,0)==ESP_OK;
}
#endif

namespace {
struct StoredProfiles {
    uint32_t magic;
    char ssid[2][33];
    char password[2][65];
};
constexpr uint32_t PROFILE_MAGIC = 0x57494632; // WIFI2
[[maybe_unused]] bool validSsid(const std::string &s) {
    // Browsers submit UTF-8; reject malformed RF names rather than replacing bytes.
    for (size_t i=0; i<s.size();) {
        const unsigned char first=s[i++];
        if (first<0x20 || first==0x7f) return false;
        if (first<0x80) continue;
        unsigned rest=0, code=0, minimum=0;
        if (first>=0xc2 && first<=0xdf) { rest=1; code=first&31; minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { rest=2; code=first&15; minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { rest=3; code=first&7; minimum=0x10000; }
        else return false;
        if (i+rest>s.size()) return false;
        while (rest--) { const unsigned char c=s[i++]; if ((c&0xc0)!=0x80) return false; code=(code<<6)|(c&63); }
        if (code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return false;
    }
    return true;
}
bool validProfile(const WifiProfile &p) {
    if (p.ssid.size() > 32 || (p.ssid.empty() && !p.password.empty())) return false;
    if (!p.password.empty() && (p.password.size() < 8 || p.password.size() > 63)) return false;
    for (unsigned char c : p.ssid) if (c < 0x20 || c == 0x7f) return false;
    for (unsigned char c : p.password) if (c < 0x20 || c > 0x7e) return false;
    return true;
}
[[maybe_unused]] bool validStored(const StoredProfiles &s) {
    if (s.magic != PROFILE_MAGIC) return false;
    WifiProfiles p;
    for (size_t i=0;i<2;++i) {
        if (!std::memchr(s.ssid[i], 0, sizeof s.ssid[i]) || !std::memchr(s.password[i], 0, sizeof s.password[i])) return false;
        p[i].ssid=s.ssid[i]; p[i].password=s.password[i]; if (!validProfile(p[i])) return false;
    }
    return !p[0].ssid.empty() || !p[1].ssid.empty();
}
WifiProfiles defaults() {
    return {{{WIFI_HOME_SSID ? WIFI_HOME_SSID : "", WIFI_HOME_PASSWORD ? WIFI_HOME_PASSWORD : ""},
             {WIFI_IPHONE_SSID ? WIFI_IPHONE_SSID : "", WIFI_IPHONE_PASSWORD ? WIFI_IPHONE_PASSWORD : ""}}};
}
WifiProfiles loadProfiles() {
    WifiProfiles profiles = defaults();
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0; StoredProfiles stored{}; size_t size = sizeof stored;
    if (nvs_open("wifi", NVS_READONLY, &handle) == ESP_OK && nvs_get_blob(handle, "profiles", &stored, &size) == ESP_OK && size == sizeof stored && validStored(stored))
        for (size_t i=0;i<2;++i) { profiles[i].ssid=stored.ssid[i]; profiles[i].password=stored.password[i]; }
    if (handle) nvs_close(handle);
#endif
    return profiles;
}
}

static void eventHandler(void *, esp_event_base_t base, int32_t id, void *data) {
#ifdef ESP_PLATFORM
    if (base == PORTAL_SCAN_BARRIER) {
        if (data && *static_cast<unsigned *>(data)==s_scan_generation.load() && s_portal_radio.load()) s_scan_radio_ready=true;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_scan_armed.exchange(false) && s_portal_radio.load() && s_scan_radio_ready.load()) {
            s_scan_status = data ? static_cast<wifi_event_sta_scan_done_t *>(data)->status : 1;
            s_scan_done.store(true);
        }
        return;
    }
#endif
    if (s_portal_radio.load()) return; // AP+STA is scan-only; ignore late station IP events.
    if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
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
    if (!esp_netif_create_default_wifi_sta()) return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
#ifdef ESP_PLATFORM
    // The explicit profiles blob owns station credentials; avoid implicit driver flash writes.
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
#endif
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr) != ESP_OK) return false;
    if (esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr) != ESP_OK) return false;
#ifdef ESP_PLATFORM
    if (esp_event_handler_register(PORTAL_SCAN_BARRIER,0,&eventHandler,nullptr)!=ESP_OK) return false;
#endif
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) return false;
    if (esp_wifi_start() != ESP_OK) return false;
    wifi_started_ = true;

    s_events = xEventGroupCreate();
    initialized_ = s_events != nullptr;
    return initialized_;
}

const WifiProfiles &WifiManager::profiles() const {
    if (!profiles_loaded_) { profiles_=loadProfiles(); profiles_loaded_=true; }
    return profiles_;
}

std::vector<std::string> WifiManager::savedSsids() const {
    const auto &saved=profiles();
    return {saved[0].ssid, saved[1].ssid};
}

bool WifiManager::saveProfileEdits(const WifiProfiles &edits, const std::array<bool,2> &open) {
    WifiProfiles merged=edits;
    const auto &old=profiles();
    for (size_t i=0;i<2;++i) {
        if (open[i] && !edits[i].password.empty()) return false;
        if (edits[i].ssid.empty() || open[i]) merged[i].password.clear();
        else if (edits[i].password.empty()) {
            if (edits[i].ssid != old[i].ssid) return false;
            merged[i].password=old[i].password;
        }
    }
    return saveProfiles(merged);
}

bool WifiManager::saveProfiles(const WifiProfiles &profiles) {
    this->profiles(); // Capture last known good values before any NVS write.
    bool any = false;
    for (const auto &profile : profiles) { if (!validProfile(profile)) return false; any |= !profile.ssid.empty(); }
    if (!any) return false;
#ifndef ESP_PLATFORM
    return false;
#else
    StoredProfiles stored{};
    stored.magic = PROFILE_MAGIC;
    for (size_t i = 0; i < 2; ++i) {
        std::memcpy(stored.ssid[i], profiles[i].ssid.data(), profiles[i].ssid.size());
        std::memcpy(stored.password[i], profiles[i].password.data(), profiles[i].password.size());
    }
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = nvs_set_blob(handle, "profiles", &stored, sizeof(stored)) == ESP_OK;
    if (ok) ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (ok) profiles_=profiles; // Never activate a write whose commit reported failure.
    return ok;
#endif
}

bool WifiManager::startPortalWifi(const char *ssid, const char *password) {
    if (!ssid || !password || std::strlen(ssid) == 0 || std::strlen(ssid) > 32 ||
        std::strlen(password) < 8 || std::strlen(password) > 63) return false;
#ifndef ESP_PLATFORM
    return false;
#else
    if (!(initialized_ || init())) return false;
    const esp_err_t disconnect = esp_wifi_disconnect();
    if (disconnect != ESP_OK && disconnect != ESP_ERR_WIFI_NOT_CONNECT) return false;
    if (esp_wifi_stop() != ESP_OK && wifi_started_) return false;
    wifi_started_ = false;
    portal_active_ = true; // Roll back partial transitions through one path.
    s_portal_radio = true; s_scan_armed = false; s_scan_done = false;
    scan_state_ = WifiScanState::Idle; scan_error_.clear();
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) { stopPortalWifi(); return false; }
    if (!ap_netif_) ap_netif_ = esp_netif_create_default_wifi_ap();
    if (!ap_netif_) { stopPortalWifi(); return false; }
    wifi_config_t cfg{};
    std::memcpy(cfg.ap.ssid, ssid, std::strlen(ssid));
    std::strncpy(reinterpret_cast<char *>(cfg.ap.password), password, sizeof(cfg.ap.password) - 1);
    cfg.ap.ssid_len = std::strlen(ssid);
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = 2;
    if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK || esp_wifi_start() != ESP_OK) {
        stopPortalWifi(); return false;
    }
    esp_netif_ip_info_t info{};
    char address[16]{};
    if (esp_netif_get_ip_info(static_cast<esp_netif_t *>(ap_netif_), &info) != ESP_OK) { stopPortalWifi(); return false; }
    std::snprintf(address, sizeof address, IPSTR, IP2STR(&info.ip));
    if (std::strcmp(address,"192.168.4.1")) { stopPortalWifi(); return false; }
    xEventGroupClearBits(s_events, GOT_IP);
    wifi_started_ = true;
    if (!scanBarrier()) { stopPortalWifi(); return false; }
    return true;
#endif
}

bool WifiManager::stopPortalWifi() {
    if (!portal_active_) return true;
    bool ok = true;
    s_portal_radio = false; s_scan_armed = false; s_scan_done = false;
    s_scan_radio_ready=false; ++s_scan_generation;
    scan_state_ = WifiScanState::Idle; scan_error_.clear();
#ifdef ESP_PLATFORM
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    ok = esp_wifi_stop() == ESP_OK;
    const bool station = esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK;
    wifi_started_ = station && esp_wifi_start() == ESP_OK;
    ok = ok && station && wifi_started_;
    xEventGroupClearBits(s_events, GOT_IP);
#endif
    portal_active_ = false;
    return ok;
}

bool WifiManager::beginPortalScan() {
    if (!portal_active_ || scan_state_ == WifiScanState::Scanning || scan_state_ == WifiScanState::Queued) return false;
    scan_error_.clear(); s_scan_done = false;
#ifdef ESP_PLATFORM
    if (!s_scan_radio_ready.load()) {
        scan_state_=WifiScanState::Queued; scan_deadline_=esp_timer_get_time()+15000000; return true;
    }
    esp_wifi_clear_ap_list();
    wifi_scan_config_t config{}; config.show_hidden = true;
    s_scan_armed = true;
    const esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        s_scan_armed = false; scan_state_ = WifiScanState::Error;
        scan_error_ = "Wi-Fi scan start failed (" + std::to_string(err) + "). Retry or enter the SSID manually.";
        return false;
    }
    scan_deadline_ = esp_timer_get_time() + 15000000;
    scan_state_ = WifiScanState::Scanning;
    return true;
#else
    scan_state_ = WifiScanState::Error; scan_error_ = "Wi-Fi scanning requires the device radio.";
    return false;
#endif
}

WifiScanState WifiManager::pollPortalScan(WifiScanResults &results, std::string &error) {
    (void)results; // Non-IDF host build has no radio.
    error = scan_error_;
    if (!portal_active_) return WifiScanState::Idle;
#ifdef ESP_PLATFORM
    if (scan_state_ == WifiScanState::Queued) {
        if (!s_scan_radio_ready.load()) {
            if (esp_timer_get_time()<scan_deadline_) return scan_state_;
            scan_state_=WifiScanState::Error; scan_error_="Wi-Fi radio is not ready. Close the portal and retry.";
            error=scan_error_; return scan_state_;
        }
        scan_state_=WifiScanState::Idle; beginPortalScan();
    }
    if (scan_state_ != WifiScanState::Scanning) { error=scan_error_; return scan_state_; }
    if (!s_scan_done.exchange(false)) {
        if (esp_timer_get_time() < scan_deadline_) return scan_state_;
        s_scan_armed = false; esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); scanBarrier();
        scan_error_ = "Wi-Fi scan timed out. Retry or enter the SSID manually.";
    } else if (s_scan_status.load()) {
        scan_error_ = "Wi-Fi scan failed (" + std::to_string(s_scan_status.load()) + "). Retry or enter the SSID manually.";
        esp_wifi_clear_ap_list();
    } else {
        // IDF returns signal-sorted records. Bound extraction before allocation.
        std::array<wifi_ap_record_t, WIFI_SCAN_LIMIT> records{};
        uint16_t count = 0;
        esp_err_t err = esp_wifi_scan_get_ap_num(&count);
        count = std::min<uint16_t>(count, records.size());
        if (err == ESP_OK && count) err = esp_wifi_scan_get_ap_records(&count, records.data());
        esp_wifi_clear_ap_list(); results.clear();
        if (err != ESP_OK) scan_error_ = "Wi-Fi scan results failed (" + std::to_string(err) + "). Retry.";
        else {
            for (size_t i=0; i<count; ++i) {
                const auto &r = records[i];
                const std::string ssid(reinterpret_cast<const char *>(r.ssid), strnlen(reinterpret_cast<const char *>(r.ssid), 32));
                if (ssid.empty() || r.primary < 1 || r.primary > 14) continue;
                if (!validSsid(ssid)) continue;
                auto found = std::find_if(results.begin(), results.end(), [&](const WifiScanNetwork &n){return n.ssid==ssid;});
                WifiScanNetwork n{ssid, r.rssi, r.primary, static_cast<unsigned>(r.authmode)};
                if (found == results.end()) results.push_back(n);
                else if (n.rssi > found->rssi) *found = n;
            }
            std::sort(results.begin(), results.end(), [](const WifiScanNetwork &a,const WifiScanNetwork &b){return a.rssi!=b.rssi ? a.rssi>b.rssi : a.ssid<b.ssid;});
            scan_state_ = WifiScanState::Complete; return scan_state_;
        }
    }
    scan_state_ = WifiScanState::Error; error = scan_error_; results.clear();
#endif
    return scan_state_;
}

bool WifiManager::connectOne(const char *ssid, const char *password, uint32_t timeout_ms) {
    if (!ssid || !ssid[0]) {
        return false;
    }

    wifi_config_t cfg{};
    std::memcpy(cfg.sta.ssid, ssid, std::min(std::strlen(ssid), sizeof(cfg.sta.ssid)));
    std::strncpy(reinterpret_cast<char *>(cfg.sta.password), password ? password : "", sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
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
    if (portal_active_) return false;
    const auto &profiles = this->profiles();
    const auto report = [&](size_t index, WifiAttemptStatus status, const std::string &ip = {}) {
        if (progress) progress(index, status, ip);
    };
    const bool ready = initialized_ || init();
    bool success = false;
    for (size_t i = 0; i < 2; ++i) {
        if (profiles[i].ssid.empty()) {
            report(i, WifiAttemptStatus::Disabled);
        } else if (success) {
            report(i, WifiAttemptStatus::Skipped);
        } else if (!ready) {
            report(i, WifiAttemptStatus::Failed);
        } else {
            report(i, WifiAttemptStatus::Connecting);
            success = connectOne(profiles[i].ssid.c_str(), profiles[i].password.c_str(), config::WIFI_CONNECT_TIMEOUT_MS);
            report(i, success ? WifiAttemptStatus::Connected : WifiAttemptStatus::Failed,
                success ? ipAddress() : std::string{});
        }
    }
    if (success) return true;

    ESP_LOGW(TAG, "No configured Wi-Fi available");
    return false;
}

bool WifiManager::connected() const {
    if (portal_active_ || !initialized_ || !(xEventGroupGetBits(s_events) & GOT_IP)) {
        return false;
    }
    wifi_ap_record_t ap{};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

std::string WifiManager::currentSsid() const {
    if (!connected()) return {};
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return {};
    }
    return {reinterpret_cast<const char *>(ap.ssid), strnlen(reinterpret_cast<const char *>(ap.ssid), sizeof ap.ssid)};
}

std::string WifiManager::ipAddress() const {
    if (portal_active_ || !initialized_ || !(xEventGroupGetBits(s_events) & GOT_IP)) return {};
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
    if (std::strcmp(buffer,"0.0.0.0")==0) return {};
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
