#include "display/ui.h"

#include <algorithm>
#include <vector>
#include "display/text_layout.h"
#include "display/ui_icons.h"

namespace display {

void Ui::header(int wifi, const std::string &title, icons::Icon icon) {
    icons::draw(display_, icon, 8, 7);
    display_.drawText(30, 8, text::clipped(title, 12));
    icons::draw(display_, icons::Icon::Wifi, 146, 7);
    if (wifi < 0) display_.drawText(163, 8, "?");
    else if (!wifi) {
        for (int i = 0; i < 16; ++i) display_.drawPixel(146 + i, 22 - i);
    }
    if (sd_ >= 0) {
        icons::draw(display_, icons::Icon::Card, 176, 7);
        if (!sd_) {
            display_.drawHLine(175, 15, 17);
            display_.drawHLine(175, 16, 17);
        }
    }
    display_.drawHLine(8, 27, 184);
}

void Ui::footer(const std::string &short_action, const std::string &long_action) {
    display_.drawHLine(8, 164, 184);
    icons::disc(display_, 14, 179, 5);
    icons::draw(display_, icons::Icon::Clock, 104, 172);
    display_.drawVLine(99, 170, 26);
    display_.drawText(24, 170, "Short");
    display_.drawText(24, 184, text::clipped(short_action, 9));
    display_.drawText(124, 170, "Long");
    display_.drawText(124, 184, text::clipped(long_action, 8));
}

void Ui::status(const std::string &label) {
    display_.drawText(8, 152, text::clipped(label, 23));
}

void Ui::row(int y, icons::Icon icon, const std::string &label, const std::string &value, bool selected) {
    if (selected) {
        display_.fillRect(10, y, 174, 20);
        display_.fillRect(8, y + 2, 178, 16);
    }
    icons::draw(display_, icon, 12, y + 2, 1, !selected);
    const auto right = text::clipped(value, 6);
    const size_t room = right.empty() ? 18 : 18 - right.size() - 1;
    display_.drawText(34, y + 4, text::clipped(label, room), 1, !selected);
    display_.drawText(180 - static_cast<int>(right.size()) * 8, y + 4, right, 1, !selected);
}

void Ui::wrappedText(int x, int y, int max_chars, int max_lines, const std::string &text) {
    auto lines = text::wrap(text, static_cast<size_t>(max_chars));
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.resize(max_lines);
        auto &last = lines.back();
        if (last.size() > static_cast<size_t>(max_chars - 3)) last.resize(max_chars - 3);
        last += "...";
    }
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        display_.drawText(x, y + i * 15, lines[i], 1);
    }
}

void Ui::showBoot() {
    display_.clear();
    display_.drawText(28, 62, "VOICE", 3);
    display_.drawText(28, 102, "NOTES", 3);
    display_.drawText(44, 150, "ESP32-S3", 1);
    display_.refresh();
}

void Ui::showBootWifi(BootWifiStatus home, BootWifiStatus hotspot, const std::string &ip) {
    const auto label = [](BootWifiStatus state) {
        switch (state) {
            case BootWifiStatus::Pending: return "Pending";
            case BootWifiStatus::Connecting: return "Connecting";
            case BootWifiStatus::Connected: return "Connected";
            case BootWifiStatus::Failed: return "Failed";
            case BootWifiStatus::Skipped: return "Skipped";
            case BootWifiStatus::Disabled: return "Disabled";
        }
        return "Unknown";
    };
    display_.clear();
    display_.drawText(12, 8, "VOICE NOTES", 2);
    display_.drawText(8, 38, "Starting Wi-Fi");
    display_.drawHLine(8, 55, 184);
    display_.drawText(8, 64, "Home Wi-Fi");
    display_.drawText(24, 82, label(home));
    display_.drawHLine(8, 104, 184);
    display_.drawText(8, 114, "Hotspot");
    display_.drawText(24, 132, label(hotspot));
    display_.drawHLine(8, 154, 184);
    if (!ip.empty()) display_.drawText(8, 166, text::clipped("IP " + ip, 23));
    display_.refresh();
}

void Ui::showIdle(const std::string &note_id, const std::string &text, size_t pending, bool wifi) {
    display_.clear();
    header(wifi);
    if (note_id.empty() && text.empty()) {
        icons::disc(display_, 100, 90, 39);
        icons::draw(display_, icons::Icon::Mic, 76, 66, 3, false);
        display_.drawText(76, 136, "RECORD");
        status(pending ? std::to_string(pending) + " pending" : "Your first note");
        footer("Record", "Settings");
        display_.refresh();
        return;
    }
    display_.drawText(8, 32, note_id.empty() ? "Your first note" : "Note " + text::clipped(note_id, 18), 1);

    if (text.empty()) {
        display_.drawText(18, 82, "Audio on SD card", 1);
        display_.drawText(18, 102, "Hold for menu", 1);
    } else {
        wrappedText(8, 48, 23, 7, text);
    }

    status(pending ? std::to_string(pending) + " pending" : (wifi ? "Wi-Fi" : "Offline"));
    footer("Record", "Settings");
    display_.refresh();
}

void Ui::showRecording(const std::string &note_id) {
    recording(note_id, "", -1);
}

void Ui::showLiveRecording(const std::string &note_id, const std::string &text, bool wifi) {
    recording(note_id, text, wifi ? 1 : 0);
}

void Ui::recording(const std::string &note_id, const std::string &text, int wifi) {
    display_.clear();
    header(wifi, "REC", icons::Icon::Mic);
    display_.drawText(8, 32, text::clipped(note_id, 23), 1);
    if (text.empty()) {
        icons::disc(display_, 100, 88, 32);
        icons::draw(display_, icons::Icon::Mic, 84, 71, 2, false);
        display_.drawText(20, 132, "Speak, I am listening");
    } else {
        const auto lines = text::wrap(text, 23);
        const size_t start = lines.size() > 7 ? lines.size() - 7 : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            display_.drawText(8, 48 + static_cast<int>(i - start) * 15, lines[i], 1);
        }
    }
    status(wifi == 0 ? "Offline / local" : (wifi > 0 ? "Wi-Fi" : "Local audio"));
    footer("Stop", "Stop");
    display_.refresh();
}

void Ui::showOffline(size_t pending, const std::string &last_id, const std::string &last_text) {
    showIdle(last_id, last_text, pending, false);
}

void Ui::showSyncing(size_t current, size_t total, const std::string &note_id) {
    display_.clear();
    header(wifi_, "Sync", icons::Icon::Sync);
    display_.drawText(8, 32, "Syncing", 1);
    display_.drawText(8, 64, text::clipped(std::to_string(current) + "/" + std::to_string(total), 23), 1);
    display_.drawRect(8, 100, 184, 12);
    if (total > 0) {
        const double ratio = static_cast<double>(std::min(current, total)) / static_cast<double>(total);
        const int fill = static_cast<int>(180.0 * ratio);
        display_.fillRect(10, 102, fill, 8);
    }
    display_.drawText(8, 126, "Sending to transcriber", 1);
    status("Note " + text::clipped(note_id, 18));
    footer("Wait", "Wait");
    display_.refresh();
}

void Ui::showMenu(size_t selected, uint8_t refresh_limit) {
    static const char *items[] = {"Notes", "Wi-Fi", "Sync", "Storage", "About", "Back", "Full refresh"};
    static const icons::Icon symbols[] = {icons::Icon::Note, icons::Icon::Wifi, icons::Icon::Sync,
        icons::Icon::Card, icons::Icon::Info, icons::Icon::Back, icons::Icon::Sync};
    const std::string values[] = {">", wifi_ < 0 ? "?" : (wifi_ ? "Yes" : "No"), "Auto",
        sd_ < 0 ? "?" : (sd_ ? "SD" : "No SD"), ">", "", refresh_limit ? std::to_string(refresh_limit) : "Only"};
    display_.clear();
    header(wifi_, "Settings", icons::Icon::Gear);
    selected %= MENU_ITEMS;
    // Six rows fit; scroll the seventh above the footer.
    const size_t start = selected >= 6 ? selected - 5 : 0;
    for (size_t i = start; i < std::min(start + 6, MENU_ITEMS); ++i)
        row(32 + static_cast<int>(i - start) * 21, symbols[i], items[i], values[i], i == selected);
    footer("Next", "Select");
    display_.refresh();
}

void Ui::showRefreshInterval(uint8_t partial_limit) {
    display_.clear();
    header(wifi_, "Full refresh", icons::Icon::Sync);
    display_.drawText(8, 42, "Clean screen after");
    display_.drawText(8, 65, partial_limit ? std::to_string(partial_limit) + " partial updates" : "Full only");
    wrappedText(8, 92, 23, 3, "More updates: less flashing, more ghosting.");
    status("Applies after saving");
    footer("Change", "Save");
    display_.refresh();
}

void Ui::showSavedNotes(const std::vector<SavedNote> &notes, size_t selected) {
    display_.clear();
    header(wifi_, "Notes", icons::Icon::Note);
    selected = std::min(selected, notes.size()); // Extra entry always goes back.
    if (notes.empty()) {
        icons::draw(display_, icons::Icon::Note, 84, 48, 2);
        display_.drawText(68, 90, "No notes");
        row(128, icons::Icon::Back, "Back", "", true);
    } else {
        constexpr size_t visible = 5;
        const size_t start = selected / visible * visible;
        const size_t end = std::min(start + visible, notes.size() + 1);
        for (size_t i = start; i < end; ++i) {
            const bool back = i == notes.size();
            const auto icon = back ? icons::Icon::Back : (notes[i].transcribed ? icons::Icon::Note : icons::Icon::Mic);
            row(34 + static_cast<int>(i - start) * 24, icon, back ? "Back" : notes[i].id, "", i == selected);
        }
        // Scroll thumb uses pages, including the Return entry; no invented timestamps.
        const size_t pages = (notes.size() + visible) / visible;
        const int thumb = std::max(6, 110 / static_cast<int>(pages));
        const int offset = pages > 1 ? static_cast<int>((110 - thumb) * (selected / visible) / (pages - 1)) : 0;
        display_.drawRect(189, 34, 3, 112);
        display_.fillRect(189, 35 + offset, 3, thumb);
        status(std::to_string(notes.size()) + " notes");
    }
    footer("Next", "Open");
    display_.refresh();
}

size_t Ui::notePageCount(const std::string &text) {
    return std::max(size_t{1}, (text::wrap(text, 23).size() + 6) / 7);
}

void Ui::showNote(const std::string &id, const std::string &text, size_t page) {
    display_.clear();
    header(wifi_, "Reader", icons::Icon::Note);
    display_.drawText(8, 32, text::clipped(id, 23));
    const auto lines = text::wrap(text, 23);
    const size_t pages = std::max(size_t{1}, (lines.size() + 6) / 7);
    page = std::min(page, pages - 1);
    const size_t first = page * 7;
    if (lines.empty()) display_.drawText(8, 48, "Empty text");
    for (size_t i = first; i < std::min(first + 7, lines.size()); ++i)
        display_.drawText(8, 48 + static_cast<int>(i - first) * 15, lines[i]);
    status("Page " + std::to_string(page + 1) + "/" + std::to_string(pages));
    footer("Page +", "Back");
    display_.refresh();
}

void Ui::showInfo(const std::string &title, const std::string &message) {
    display_.clear();
    header(wifi_, title, icons::Icon::Info);
    wrappedText(8, 36, 23, 8, message);
    footer("Back", "Back");
    display_.refresh();
}

void Ui::showError(const std::string &title, const std::string &message, bool recoverable) {
    display_.clear();
    header(wifi_, "Warning", icons::Icon::Warning);
    icons::draw(display_, icons::Icon::Warning, 84, 36, 2);
    display_.drawText(8, 76, text::clipped(title, 23));
    wrappedText(8, 98, 23, 4, message);
    if (recoverable) footer("Record", "Settings");
    else {
        display_.drawHLine(8, 164, 184);
        display_.drawText(72, 180, "Restart");
    }
    display_.refresh();
}

void Ui::showStorageError() {
    display_.clear();
    sd_ = 0;
    header(wifi_, "Warning", icons::Icon::Card);
    icons::draw(display_, icons::Icon::Warning, 68, 46, 4);
    display_.drawText(44, 116, "SD unavailable");
    display_.drawText(28, 140, "Insert FAT32 card");
    footer("Menu", "Settings");
    display_.refresh();
}

} // namespace display
