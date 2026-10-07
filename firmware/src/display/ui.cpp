#include "display/ui.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "display/text_layout.h"
#include "display/ui_icons.h"
#include "display/wifi_qr.h"

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

void Ui::scrollbar(int y, int height, size_t total, size_t first, size_t last) {
    first = std::min(first, total);
    last = std::min(last, total);
    if (height < 3 || last <= first || last - first >= total) return;
    const int inner = height - 2;
    // Project the actual displayed range, including Back and partial last pages.
    const int top = std::min(inner - 1, static_cast<int>(std::floor(static_cast<double>(first) / total * inner)));
    const int bottom = std::min(inner, std::max(top + 1, static_cast<int>(std::ceil(static_cast<double>(last) / total * inner))));
    display_.drawRect(189, y, 3, height);
    display_.fillRect(189, y + 1 + top, 3, bottom - top);
}

void Ui::row(int y, icons::Icon icon, const std::string &label, const std::string &value, bool selected, bool child) {
    if (selected) {
        display_.fillRect(10, y, 174, 20);
        display_.fillRect(8, y + 2, 178, 16);
    }
    icons::draw(display_, icon, 12, y + 2, 1, !selected);
    const auto right = text::clipped(value, child ? 5 : 6);
    const size_t room = 18 - (child ? 2 : 0) - (right.empty() ? 0 : right.size() + 1);
    display_.drawText(34, y + 4, text::clipped(label, room), 1, !selected);
    display_.drawText((child ? 164 : 180) - static_cast<int>(right.size()) * 8, y + 4, right, 1, !selected);
    if (child) display_.drawText(172, y + 4, ">", 1, !selected);
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

void Ui::bootCircle(size_t completed) {
    completed = std::min(completed, size_t{4});
    constexpr double pi = 3.14159265358979323846;
    for (int y = -33; y <= 33; ++y) {
        for (int x = -33; x <= 33; ++x) {
            const int radius2 = x*x + y*y;
            double angle = std::atan2(static_cast<double>(x), static_cast<double>(-y));
            if (angle < 0) angle += 2*pi;
            if ((radius2 >= 32*32 && radius2 < 33*33) ||
                (radius2 >= 27*27 && radius2 < 31*31 && completed && angle < completed*pi/2))
                display_.drawPixel(100+x, 78+y);
        }
    }
    icons::draw(display_, icons::Icon::Sync, 92, 56);
    display_.drawText(88, 82, std::to_string(completed) + "/4");
}

void Ui::showBootProgress(size_t completed, const std::string &stage, const std::string &detail) {
    display_.clear();
    header(wifi_, "Starting", icons::Icon::Sync);
    bootCircle(completed);
    const auto title = text::clipped(stage, 23);
    display_.drawText(100-static_cast<int>(title.size())*4, 118, title);
    wrappedText(8, 138, 23, 2, detail);
    display_.drawHLine(8, 174, 184);
    display_.drawText(8, 182, "Settings stay available");
    display_.refresh();
}

void Ui::showBoot() {
    sd_ = -1; // No mount result exists yet.
    showBootProgress(0, "SD card", "Initializing");
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
    header(wifi_, "Starting", icons::Icon::Sync);
    bootCircle(2);
    display_.drawText(80, 118, "Wi-Fi");
    display_.drawText(8, 138, "Home Wi-Fi");
    display_.drawText(96, 138, label(home));
    display_.drawText(8, 154, "Hotspot");
    display_.drawText(96, 154, label(hotspot));
    display_.drawHLine(8, 174, 184);
    if (!ip.empty()) display_.drawText(8, 182, text::clipped("IP " + ip, 23));
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
    footer("Wait", "Cancel");
    display_.refresh();
}

void Ui::showSyncStopping() {
    display_.clear();
    header(wifi_, "Sync", icons::Icon::Sync);
    display_.drawText(8, 38, "Stopping sync");
    wrappedText(8, 64, 23, 5, "Waiting for HTTP to stop. Pending audio stays on SD. Server work may finish.");
    footer("Wait", "Wait");
    display_.refresh();
}

void Ui::showMenu(size_t selected, uint8_t refresh_limit) {
    static const char *items[] = {"Notes", "Sync", "Wi-Fi", "Storage", "Full refresh", "About", "Back"};
    static const icons::Icon symbols[] = {icons::Icon::Note, icons::Icon::Sync, icons::Icon::Wifi,
        icons::Icon::Card, icons::Icon::Sync, icons::Icon::Info, icons::Icon::Back};
    const std::string values[] = {"", "Auto", wifi_ < 0 ? "?" : (wifi_ ? "Yes" : "No"),
        sd_ < 0 ? "?" : (sd_ ? "SD" : "No SD"), refresh_limit ? std::to_string(refresh_limit) : "Only", "", ""};
    static const bool children[] = {true, true, true, true, true, true, false};
    display_.clear();
    header(wifi_, "Settings", icons::Icon::Gear);
    selected %= MENU_ITEMS;
    // Six rows fit; scroll the seventh above the footer.
    const size_t start = selected >= 6 ? selected - 5 : 0;
    for (size_t i = start; i < std::min(start + 6, MENU_ITEMS); ++i)
        row(32 + static_cast<int>(i - start) * 21, symbols[i], items[i], values[i], i == selected, children[i]);
    scrollbar(32, 126, MENU_ITEMS, start, std::min(start + 6, MENU_ITEMS));
    footer("Next", "Select");
    display_.refresh();
}

void Ui::showSubmenu(const std::string &title, const std::vector<std::string> &entries, size_t selected,
                     const std::vector<bool> &children) {
    display_.clear();
    header(wifi_, title, icons::Icon::Gear);
    if (entries.empty()) {
        display_.drawText(8, 52, "No options");
    } else {
        selected %= entries.size();
        constexpr size_t visible = 6;
        const size_t start = selected >= visible ? selected - visible + 1 : 0;
        const size_t end = std::min(start + visible, entries.size());
        for (size_t i = start; i < end; ++i)
            row(32 + static_cast<int>(i - start) * 21, icons::Icon::Back, entries[i], "", i == selected,
                i < children.size() && children[i]);
        scrollbar(32, 126, entries.size(), start, end);
    }
    footer("Next", "Select");
    display_.refresh();
}

void Ui::showCancelConfirmation(bool selected, bool all, const std::string &id) {
    display_.clear(); header(wifi_,"Cancel sync?",icons::Icon::Sync);
    if(all) wrappedText(8,36,23,2,"All pending transcriptions");
    else wrappedText(8,36,23,2,id);
    wrappedText(8,70,23,2,"Keep audio on SD. No transcript created.");
    row(112,icons::Icon::Back,"Back","",!selected);
    row(136,icons::Icon::Mic,"Cancel pending","",selected);
    footer("Change",selected ? "Cancel" : "Back"); display_.refresh();
}

void Ui::storageGauge(bool known, uint64_t total, uint64_t free) {
    known=known && total && free<=total;
    display_.drawRect(8,114,180,12);
    if (!known) { display_.drawText(8,136,"Usage unknown"); return; }
    const uint64_t used=total-free;
    // Floating projection avoids uint64 multiplication overflow, including huge cards.
    const int fill=static_cast<int>(176.0L*static_cast<long double>(used)/total);
    display_.fillRect(10,116,std::min(176,std::max(0,fill)),8);
    display_.drawText(8,132,text::clipped(std::to_string(used/1048576) + " MiB used",23));
    display_.drawText(8,146,text::clipped("of " + std::to_string(total/1048576) + " MiB",23));
}
void Ui::showStorageMenu(size_t selected, bool known, uint64_t total, uint64_t free) {
    display_.clear(); header(wifi_,"Storage",icons::Icon::Card);
    row(32,icons::Icon::Card,"Status / retry mount","",selected%3==0,true);
    row(53,icons::Icon::Warning,"Format SD","",selected%3==1,true);
    row(74,icons::Icon::Back,"Back","",selected%3==2,false);
    storageGauge(known,total,free);
    footer("Next","Select"); display_.refresh();
}
void Ui::showStorageStatus(const std::string &message, bool known, uint64_t total, uint64_t free) {
    display_.clear(); header(wifi_,"Storage",icons::Icon::Card);
    wrappedText(8,34,23,5,message);
    storageGauge(known,total,free);
    footer("Back","Back"); display_.refresh();
}

void Ui::showFormatConfirmation(bool erase_selected) {
    display_.clear();
    header(wifi_, "Format SD", icons::Icon::Warning);
    display_.drawText(8, 38, "Erase all card data?");
    wrappedText(8, 60, 23, 3, "This erases ALL notes and recordings on the card.");
    row(112, icons::Icon::Back, "Cancel", "", !erase_selected);
    row(136, icons::Icon::Warning, "Erase SD", "", erase_selected);
    footer("Change", erase_selected ? "Erase" : "Cancel");
    display_.refresh();
}

void Ui::showPortal(const std::string &ssid, const std::string &password, const std::string &address) {
    display_.clear();
    header(-1, "Wi-Fi setup", icons::Icon::Wifi);
    if (!qr::drawWifi(display_, ssid, password)) {
        display_.drawText(8, 56, "QR unavailable");
        display_.drawText(8, 74, "Connect manually");
    }
    // Keep the complete 32-byte SSID on two rows, without word-wrap changing it.
    const auto manualSsid = text::ascii(ssid);
    display_.drawText(8, 120, manualSsid.substr(0, 23));
    display_.drawText(8, 134, text::clipped(manualSsid.size() > 23 ? manualSsid.substr(23) : "", 23));
    display_.drawText(8, 148, text::clipped("PW: " + password, 23));
    display_.drawText(8, 162, text::clipped(address, 23));
    display_.drawText(8, 184, "Close (10 min)");
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
        scrollbar(34, 112, notes.size() + 1, start, end);
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
