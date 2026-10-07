#pragma once

#include <string>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <array>
#include <vector>

namespace network {

enum class WifiAttemptStatus { Connecting, Connected, Failed, Skipped, Disabled };
using WifiProgressCallback = std::function<void(size_t, WifiAttemptStatus, const std::string &)>;

struct WifiProfile { std::string ssid; std::string password; };
using WifiProfiles = std::array<WifiProfile, 2>;

enum class WifiScanState { Idle, Queued, Scanning, Complete, Error };
struct WifiScanNetwork { std::string ssid; int rssi; unsigned channel; unsigned authmode; };
using WifiScanResults = std::vector<WifiScanNetwork>;
constexpr size_t WIFI_SCAN_LIMIT = 20;

class WifiManager {
public:
    bool init();
    bool connectPreferred(WifiProgressCallback progress = {});
    std::vector<std::string> savedSsids() const;
    bool saveProfiles(const WifiProfiles &profiles);
    // Empty password keeps the previous secret only for the same SSID; open is explicit.
    bool saveProfileEdits(const WifiProfiles &edits, const std::array<bool,2> &open);
    bool startPortalWifi(const char *ssid, const char *password);
    bool stopPortalWifi();
    // Main-task only. Native asynchronous scan; never connects the station.
    bool beginPortalScan();
    WifiScanState pollPortalScan(WifiScanResults &results, std::string &error);
    bool portalActive() const { return portal_active_; }
    bool connected() const;
    std::string currentSsid() const;
    std::string ipAddress() const;

private:
    const WifiProfiles &profiles() const;
    mutable WifiProfiles profiles_{};
    mutable bool profiles_loaded_ = false; // Main owns profiles, not the HTTP task.
    bool connectOne(const char *ssid, const char *password, uint32_t timeout_ms);
    void startSntp();
    bool initialized_ = false;
    bool portal_active_ = false;
    void *ap_netif_ = nullptr; // Default AP netif reused across physically triggered sessions.
    bool wifi_started_ = false;
    WifiScanState scan_state_ = WifiScanState::Idle;
    std::string scan_error_;
    int64_t scan_deadline_ = 0;
};

} // namespace network
