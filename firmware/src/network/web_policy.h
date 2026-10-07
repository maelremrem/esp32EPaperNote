#pragma once
#include <cstddef>
#include <cstring>
#include <string>
namespace network::web {
enum class State { Idle, Recording, Menu, Syncing };
enum class Command { Start, Stop, Sync, Configure, Wifi, Display, Mount, PrepareFormat, Format, Reconnect, Download };
struct SettingsWrite {
    char ssid[2][33]{}, password[2][64]{};
    bool open[2]{};
    char server_token[193]{};
    bool replace_token=false;
    unsigned partial_limit=10;
    unsigned challenge=0;
};
inline bool allowed(Command command, State state, bool stopping, bool recovery,
                    bool wifi, size_t pending, bool connecting = false) {
    if (connecting) return false;
    switch (command) {
        case Command::Wifi: case Command::Display: case Command::Mount:
        case Command::PrepareFormat: case Command::Format: case Command::Reconnect:
        case Command::Download:
        case Command::Configure: return (state == State::Idle || state == State::Menu) && !stopping;
        case Command::Start: return state == State::Idle && !recovery;
        case Command::Stop: return state == State::Recording && !stopping;
        case Command::Sync: return state == State::Idle && wifi && pending > 0;
    }
    return false;
}
// Only the literal station IPv4 address is accepted, preventing DNS rebinding.
inline bool sameDevice(const std::string &host, const std::string &origin, const std::string &ip) {
    return !ip.empty() && (host == ip || host == ip + ":80") &&
           (origin.empty() || origin == "http://" + ip || origin == "http://" + ip + ":80");
}
inline bool validNoteId(const std::string &id) {
    if(id.empty() || id.size()>96) return false;
    for(unsigned char c:id) if(!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_')) return false;
    return true;
}
inline bool validServerToken(const std::string &token) {
    if (token.size()>192) return false;
    for (unsigned char c:token) if (c<33 || c>126) return false;
    return true;
}
inline bool authenticated(const std::string &header, const std::string &token) {
    if (token.empty() || header.size() != token.size() + 7 || header.compare(0, 7, "Bearer ")) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < token.size(); ++i) difference |= static_cast<unsigned char>(header[i + 7] ^ token[i]);
    return difference == 0;
}
template <size_t N> inline void copyText(char (&out)[N], const std::string &text) {
    size_t n = text.size() < N - 1 ? text.size() : N - 1;
    if (n < text.size()) while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
    std::memcpy(out, text.data(), n);
    out[n] = 0;
}
// One slot, protected by WebServer's mutex. Reserve until main completes, not just takes.
class Mailbox {
public:
    unsigned submit(Command command) {
        if (reserved_) return 0;
        reserved_ = queued_ = true;
        command_ = command;
        if (++id_ == 0) ++id_;
        return id_;
    }
    bool take(Command &command, unsigned &id) {
        if (!queued_) return false;
        queued_ = false; command = command_; id = id_; return true;
    }
    void complete() { reserved_ = false; }
    bool busy() const { return reserved_; }
private:
    bool reserved_ = false, queued_ = false;
    unsigned id_ = 0;
    Command command_ = Command::Start;
};
} // namespace network::web
