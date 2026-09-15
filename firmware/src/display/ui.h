#pragma once

#include <string>

#include "display/epaper_display.h"

namespace display {

class Ui {
public:
    explicit Ui(EpaperDisplay &display) : display_(display) {}

    void showBoot();
    void showIdle(const std::string &note_id, const std::string &text, size_t pending, bool wifi);
    void showRecording(const std::string &note_id);
    void showOffline(size_t pending, const std::string &last_id, const std::string &last_text);
    void showSyncing(size_t current, size_t total, const std::string &note_id);
    void showMenu(size_t selected);
    void showError(const std::string &title, const std::string &message);

private:
    void header(bool wifi);
    void footer(const std::string &hint);
    void wrappedText(int x, int y, int max_chars, int max_lines, const std::string &text);

    EpaperDisplay &display_;
};

} // namespace display
