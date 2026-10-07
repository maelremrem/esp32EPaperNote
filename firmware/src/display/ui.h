#pragma once

#include <string>
#include <vector>

#include "display/epaper_display.h"
#include "display/ui_icons.h"

namespace display {

enum class BootWifiStatus { Pending, Connecting, Connected, Failed, Skipped, Disabled };

struct SavedNote {
    std::string id;
    bool transcribed = false;
};

class Ui {
public:
    explicit Ui(EpaperDisplay &display) : display_(display) {}

    static constexpr size_t MENU_ITEMS = 7;
    void setStatus(bool wifi, bool sd) { wifi_ = wifi; sd_ = sd; }

    void showBoot();
    // Four completed startup stages, not elapsed time or an animated spinner.
    void showBootProgress(size_t completed, const std::string &stage, const std::string &detail);
    // Caller supplies observed connection states; an empty IP renders no address.
    void showBootWifi(BootWifiStatus home, BootWifiStatus hotspot, const std::string &ip = "");
    void showIdle(const std::string &note_id, const std::string &text, size_t pending, bool wifi);
    void showRecording(const std::string &note_id);
    void showLiveRecording(const std::string &note_id, const std::string &text, bool wifi);
    void showOffline(size_t pending, const std::string &last_id, const std::string &last_text);
    void showSyncing(size_t current, size_t total, const std::string &note_id);
    void showSyncStopping();
    void showMenu(size_t selected, uint8_t refresh_limit = 10);
    // Explicit child-view flags; omitted/missing flags mean actions, never guessed from labels.
    void showSubmenu(const std::string &title, const std::vector<std::string> &entries, size_t selected,
                     const std::vector<bool> &children = {});
    void showFormatConfirmation(bool erase_selected);
    void showCancelConfirmation(bool cancel_selected, bool all, const std::string &id);
    void showStorageMenu(size_t selected, bool known, uint64_t total, uint64_t free);
    void showStorageStatus(const std::string &message, bool known, uint64_t total, uint64_t free);
    void showPortal(const std::string &ssid, const std::string &password, const std::string &address);
    void showRefreshInterval(uint8_t partial_limit);
    void showError(const std::string &title, const std::string &message, bool recoverable = true);
    void showStorageError();
    void showSavedNotes(const std::vector<SavedNote> &notes, size_t selected);
    void showNote(const std::string &id, const std::string &text, size_t page);
    static size_t notePageCount(const std::string &text);
    void showInfo(const std::string &title, const std::string &message);

private:
    // -1 = unknown: legacy methods have no network argument.
    void header(int wifi, const std::string &title = "Notebook", icons::Icon icon = icons::Icon::Note);
    void recording(const std::string &note_id, const std::string &text, int wifi);
    void footer(const std::string &short_action, const std::string &long_action);
    void status(const std::string &label);
    void bootCircle(size_t completed);
    void storageGauge(bool known, uint64_t total, uint64_t free);
    void scrollbar(int y, int height, size_t total, size_t first, size_t last);
    void row(int y, icons::Icon icon, const std::string &label, const std::string &value, bool selected, bool child = false);
    void wrappedText(int x, int y, int max_chars, int max_lines, const std::string &text);

    EpaperDisplay &display_;
    int wifi_ = -1;
    int sd_ = -1; // Mount result, not a hot-plug detector or free-space estimate.
};

} // namespace display
