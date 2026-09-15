#pragma once

#include <string>

namespace network {

class WifiManager {
public:
    bool init();
    bool connectPreferred();
    bool connected() const;
    std::string currentSsid() const;
    std::string ipAddress() const;

private:
    bool connectOne(const char *ssid, const char *password, uint32_t timeout_ms);
    void startSntp();
    bool initialized_ = false;
};

} // namespace network
