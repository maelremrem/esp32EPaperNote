#pragma once
#include <string>
#include <cstddef>
namespace network {
constexpr size_t SERVER_URL_MAX = 63;
// Deliberately literal IPv4 only: no DNS aliases, credentials or base paths.
inline bool validServerUrl(const std::string &url, const std::string &device_ip = {}) {
    if (url.size() > SERVER_URL_MAX) return false;
    size_t begin = url.rfind("http://", 0) == 0 ? 7 : url.rfind("https://", 0) == 0 ? 8 : 0;
    if (!begin) return false;
    size_t pos = begin;
    unsigned octets[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        size_t start = pos;
        while (pos < url.size() && url[pos] >= '0' && url[pos] <= '9') {
            if (pos - start >= 3) return false;
            octets[i] = octets[i] * 10 + unsigned(url[pos++] - '0');
        }
        if (pos == start || octets[i] > 255 || (pos - start > 1 && url[start] == '0')) return false;
        if (i < 3 && (pos == url.size() || url[pos++] != '.')) return false;
    }
    const auto ip = url.substr(begin, pos - begin);
    if (ip == device_ip || octets[0] == 0 || octets[0] == 127 || octets[0] >= 224 ||
        (octets[0] == 169 && octets[1] == 254)) return false;
    if (pos == url.size()) return true;
    if (url[pos++] != ':' || pos == url.size() || url[pos] == '0') return false;
    unsigned port = 0;
    size_t start = pos;
    while (pos < url.size()) {
        if (pos - start >= 5 || url[pos] < '0' || url[pos] > '9') return false;
        port = port * 10 + unsigned(url[pos++] - '0');
    }
    return port > 0 && port <= 65535;
}
}
