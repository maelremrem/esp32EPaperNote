#include <cassert>
#include <cstdio>
#include "network/web_policy.h"
int main() {
    using namespace network::web;
    assert(allowed(Command::Start, State::Idle, false, false, false, 0));
    assert(!allowed(Command::Start, State::Recording, false, false, true, 1));
    assert(!allowed(Command::Start, State::Menu, false, false, true, 1));
    assert(!allowed(Command::Start, State::Syncing, false, false, true, 1));
    assert(!allowed(Command::Start, State::Idle, false, true, true, 1));
    assert(allowed(Command::Stop, State::Recording, false, false, true, 0));
    assert(!allowed(Command::Stop, State::Recording, true, false, true, 0));
    assert(!allowed(Command::Stop, State::Idle, false, false, true, 1));
    assert(allowed(Command::Sync, State::Idle, false, false, true, 1));
    assert(!allowed(Command::Sync, State::Idle, false, false, false, 1));
    assert(!allowed(Command::Sync, State::Idle, false, false, true, 0));
    assert(!allowed(Command::Sync, State::Recording, false, false, true, 1));
    assert(!allowed(Command::Start, State::Idle, false, false, true, 1, true));
    assert(!allowed(Command::Sync, State::Idle, false, false, true, 1, true));
    assert(sameDevice("192.168.1.4", "", "192.168.1.4"));
    assert(sameDevice("192.168.1.4:80", "http://192.168.1.4:80", "192.168.1.4"));
    assert(sameDevice("192.168.1.4", "http://192.168.1.4", "192.168.1.4"));
    assert(!sameDevice("evil.test", "http://evil.test", "192.168.1.4"));
    assert(!sameDevice("192.168.1.4", "http://evil.test", "192.168.1.4"));
    assert(!sameDevice("192.168.1.4", "null", "192.168.1.4"));
    assert(!sameDevice("192.168.1.4", "http://192.168.1.4.evil", "192.168.1.4"));
    assert(!sameDevice("", "", ""));
    assert(authenticated("Bearer test-token", "test-token"));
    assert(!authenticated("Bearer test-tokenX", "test-token"));
    assert(!authenticated("Bearer ", ""));
    assert(!authenticated("test-token", "test-token"));
    char text[5];
    copyText(text, "abcé"); assert(std::string(text) == "abc");
    copyText(text, "ééé"); assert(std::string(text) == "éé");
    copyText(text, ""); assert(text[0] == 0);
    Mailbox mailbox;
    unsigned id = 0; Command command{};
    assert(!mailbox.take(command, id));
    assert(mailbox.submit(Command::Start) == 1);
    assert(mailbox.submit(Command::Sync) == 0);
    assert(mailbox.take(command, id) && command == Command::Start && id == 1);
    assert(!mailbox.take(command, id));
    assert(mailbox.submit(Command::Stop) == 0);
    mailbox.complete();
    assert(mailbox.submit(Command::Stop) == 2);
    puts("web state, security, UTF-8 bounds and mailbox: PASS");
}
