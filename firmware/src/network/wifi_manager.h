#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace network {

enum class WifiAttemptStatus { Connecting, Connected, Failed, Skipped, Disabled };
using WifiProgressCallback = std::function<void(size_t, WifiAttemptStatus, const std::string &)>;

class WifiManager {
public:
    bool init();
    bool connectPreferred(WifiProgressCallback progress = {});
    bool connected() const;
    std::string currentSsid() const;
    std::string ipAddress() const;

private:
    bool connectOne(const char *ssid, const char *password, uint32_t timeout_ms);
    void startSntp();
    bool initialized_ = false;
};

} // namespace network
