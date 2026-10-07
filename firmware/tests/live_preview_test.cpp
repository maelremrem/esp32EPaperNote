#include <cassert>
#include <cstdint>
#include <iostream>
#include "network/live_preview_helpers.h"

int main() {
    // Actual identifiers become a single safe path component, never a fixed placeholder.
    assert(network::live::endpoint("20261006_123456-01") == "/api/v1/live/20261006_123456-01");
    assert(network::live::endpoint("../bad").empty());
    assert(network::live::endpoint("").empty());
    assert(network::live::validPcmSize(32000));
    assert(network::live::validPcmSize(960000));
    assert(!network::live::validPcmSize(960002));
    assert(!network::live::validPcmSize(0));
    assert(!network::live::validPcmSize(3));
    std::string text;
    // French transcript fixtures deliberately preserve multilingual behavior.
    network::live::appendText(text, "bonjour", 24);
    network::live::appendText(text, "le monde", 24);
    assert(text == "bonjour le monde");
    network::live::appendText(text, "", 24);
    assert(text == "bonjour le monde");
    network::live::appendText(text, "une suite longue", 24);
    assert(text.size() <= 24);
    text.clear();
    network::live::appendText(text, "été", 3);
    assert(text == "ét"); // Never end midway through a UTF-8 code point.
    assert(network::live::validResponse("note", "note", "partial", "whistle", true, true));
    assert(!network::live::validResponse("note", "other", "partial", "whistle", true, true));
    assert(!network::live::validResponse("note", "note", "done", "whistle", true, true));
    assert(!network::live::validResponse("note", "note", "partial", "other", true, true));
    assert(!network::live::validResponse("note", "note", "partial", "whistle", false, true));
    network::live::RefreshGate gate;
    gate.reset(1000, "", true);
    assert(!gate.changed(2999, "bonjour", true));
    assert(gate.changed(3000, "bonjour", true));
    assert(!gate.changed(5000, "bonjour", true));
    assert(gate.changed(5000, "bonjour", false));
    assert(!gate.changed(6000, "suite", false));
    uint8_t pcm[8]{};
    const uint8_t first[] = {0x34, 0x12, 0x00, 0x80}; // s16le, including negative sample.
    const uint8_t second[] = {1, 0, 2, 0};
    network::live::PcmWindow window(pcm, sizeof(pcm));
    assert(window.append(0, first, sizeof(first)));
    assert(window.size() == 4 && pcm[0] == 0x34 && pcm[3] == 0x80);
    assert(window.append(1, second, sizeof(second)));
    assert(window.size() == 8 && pcm[4] == 1);
    assert(!window.append(2, second, sizeof(second))); // Never exceed fixed storage.
    window.reset();
    assert(window.append(3, first, sizeof(first)));
    assert(window.append(5, second, sizeof(second))); // Missing chunk clears prior window.
    assert(window.size() == 4 && pcm[0] == 1);
    assert(!window.append(6, first, 3));
    assert(!window.append(6, nullptr, 4));
    assert(network::live::canDiscardShortWav(3199, true));
    assert(!network::live::canDiscardShortWav(3200, true));
    assert(!network::live::canDiscardShortWav(0, false));
    assert(!network::live::canDiscardShortWav(2000, false));
    std::cout << "live preview helper tests passed\n";
}
