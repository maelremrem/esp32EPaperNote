#pragma once

#include <string>
#include <vector>

namespace display::text {

// The panel font is ASCII. Decode once before measuring, clipping or wrapping.
// A valid unsupported code point occupies one cell, never one cell per byte.
inline char nextGlyph(const char *&p) {
    const auto lead = static_cast<unsigned char>(*p++);
    if (lead < 0x80) return static_cast<char>(lead);
    int remaining = 0;
    unsigned code = 0;
    unsigned minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) { remaining = 1; code = lead & 0x1F; minimum = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { remaining = 2; code = lead & 0x0F; minimum = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { remaining = 3; code = lead & 7; minimum = 0x10000; }
    else return '?';
    for (int i = 0; i < remaining; ++i) {
        const auto byte = static_cast<unsigned char>(*p);
        if ((byte & 0xC0) != 0x80) return '?';
        code = (code << 6) | (byte & 0x3F);
        ++p;
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return '?';
    switch (code) {
        case 0xE0: case 0xE2: case 0xE4: return 'a';
        case 0xE7: return 'c';
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
        case 0xEE: case 0xEF: return 'i';
        case 0xF4: case 0xF6: return 'o';
        case 0xF9: case 0xFB: case 0xFC: return 'u';
        case 0xC0: case 0xC2: case 0xC4: return 'A';
        case 0xC7: return 'C';
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
        case 0xCE: case 0xCF: return 'I';
        case 0xD4: case 0xD6: return 'O';
        case 0xD9: case 0xDB: case 0xDC: return 'U';
        case 0xA0: return ' ';
        case 0x2018: case 0x2019: return '\'';
        case 0x2013: case 0x2014: return '-';
        default: return '?';
    }
}

inline std::string ascii(const std::string &input) {
    std::string out;
    const char *p = input.c_str();
    while (*p) {
        char c = nextGlyph(p);
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c != '\n' && (c < 32 || c > 126)) c = '?';
        out += c;
    }
    return out;
}

inline std::string clipped(const std::string &input, size_t cells) {
    auto out = ascii(input);
    for (char &c : out) if (c == '\n') c = ' ';
    if (out.size() > cells) {
        out.resize(cells);
        if (cells >= 3) out.replace(cells - 3, 3, "...");
    }
    return out;
}

inline std::vector<std::string> wrap(const std::string &input, size_t cells) {
    std::vector<std::string> lines;
    if (cells == 0) return lines;
    std::string line, word;
    const auto flushWord = [&]() {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > cells) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += ' ';
        while (word.size() > cells) {
            lines.push_back(word.substr(0, cells));
            word.erase(0, cells);
        }
        line += word;
        word.clear();
    };
    for (char c : ascii(input)) {
        if (c == ' ' || c == '\n') {
            flushWord();
            if (c == '\n') { lines.push_back(line); line.clear(); }
        } else word += c;
    }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace display::text
