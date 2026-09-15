#include "display/ui.h"

#include <algorithm>
#include <vector>

namespace display {

void Ui::header(bool wifi) {
    display_.drawText(8, 6, "NOTES", 2);
    display_.drawText(124, 10, wifi ? "W" : "X", 1);
    display_.drawText(145, 10, "SD", 1);
    display_.drawText(174, 10, "BAT", 1);
    display_.drawHLine(6, 32, 188);
}

void Ui::footer(const std::string &hint) {
    display_.drawHLine(6, 176, 188);
    display_.drawText(8, 182, hint, 1);
}

void Ui::wrappedText(int x, int y, int max_chars, int max_lines, const std::string &text) {
    std::vector<std::string> lines;
    std::string line;
    std::string word;

    auto flushWord = [&]() {
        if (word.empty()) return;
        if (line.empty()) {
            line = word;
        } else if (static_cast<int>(line.size() + 1 + word.size()) <= max_chars) {
            line += " " + word;
        } else {
            lines.push_back(line);
            line = word;
        }
        word.clear();
    };

    for (char c : text) {
        if (c == ' ' || c == '\n') {
            flushWord();
            if (c == '\n' && !line.empty()) {
                lines.push_back(line);
                line.clear();
            }
        } else {
            word.push_back(c);
        }
        if (static_cast<int>(lines.size()) >= max_lines) break;
    }
    flushWord();
    if (!line.empty() && static_cast<int>(lines.size()) < max_lines) lines.push_back(line);

    if (static_cast<int>(lines.size()) > max_lines) lines.resize(max_lines);
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string out = lines[i];
        if (i == max_lines - 1 && out.size() >= static_cast<size_t>(max_chars - 2)) {
            out.resize(max_chars - 3);
            out += "...";
        }
        display_.drawText(x, y + i * 16, out, 1);
    }
}

void Ui::showBoot() {
    display_.clear();
    display_.drawText(28, 62, "VOICE", 3);
    display_.drawText(28, 102, "NOTES", 3);
    display_.drawText(44, 150, "ESP32-S3", 1);
    display_.refresh();
}

void Ui::showIdle(const std::string &note_id, const std::string &text, size_t pending, bool wifi) {
    display_.clear();
    header(wifi);
    display_.drawText(8, 43, note_id.empty() ? "AUCUNE NOTE" : "NOTE " + note_id, 1);
    display_.drawHLine(8, 60, 184);

    if (text.empty()) {
        display_.drawText(18, 82, "2x = nouvelle note", 1);
        display_.drawText(18, 102, "Maintien = menu", 1);
    } else {
        wrappedText(8, 72, 23, 5, text);
    }

    if (pending == 0) {
        display_.drawText(8, 156, "SYNCED", 1);
    } else {
        display_.drawText(8, 156, std::to_string(pending) + " EN ATTENTE", 1);
    }
    footer("1x suivant  2x note  hold menu");
    display_.refresh();
}

void Ui::showRecording(const std::string &note_id) {
    display_.clear();
    header(true);
    display_.drawText(25, 62, "NOUVELLE", 2);
    display_.drawText(52, 90, "NOTE", 2);
    display_.drawText(70, 124, "REC", 2);
    display_.drawText(18, 150, note_id, 1);
    footer("1x arreter");
    display_.refresh();
}

void Ui::showOffline(size_t pending, const std::string &last_id, const std::string &last_text) {
    display_.clear();
    header(false);
    display_.drawText(18, 50, "OFFLINE", 2);
    display_.drawText(8, 82, std::to_string(pending) + " notes en attente", 1);
    display_.drawText(8, 100, "En attente du reseau", 1);
    if (!last_id.empty()) {
        display_.drawText(8, 128, "NOTE " + last_id, 1);
        wrappedText(8, 144, 23, 2, last_text);
    }
    footer("1x suivant  hold sync");
    display_.refresh();
}

void Ui::showSyncing(size_t current, size_t total, const std::string &note_id) {
    display_.clear();
    header(true);
    display_.drawText(8, 48, "SYNCHRO", 2);
    display_.drawText(8, 78, std::to_string(current) + "/" + std::to_string(total), 1);
    display_.drawRect(8, 100, 184, 12);
    if (total > 0) {
        const int fill = static_cast<int>(180.0 * static_cast<double>(current) / static_cast<double>(total));
        display_.fillRect(10, 102, std::clamp(fill, 0, 180), 8);
    }
    display_.drawText(8, 126, "Envoi vers Whisper", 1);
    display_.drawText(8, 148, "NOTE " + note_id, 1);
    footer("synchronisation...");
    display_.refresh();
}

void Ui::showMenu(size_t selected) {
    static const char *items[] = {"Nouvelle note", "Forcer sync", "Historique", "Reseau"};
    display_.clear();
    header(true);
    display_.drawText(8, 44, "MENU", 2);

    for (size_t i = 0; i < 4; ++i) {
        const int y = 76 + static_cast<int>(i) * 23;
        if (i == selected) {
            display_.fillRect(8, y - 3, 184, 19, true);
            display_.drawText(14, y, std::string("> ") + items[i], 1, false);
        } else {
            display_.drawText(14, y, items[i], 1);
        }
    }
    footer("1x suivant  hold choisir");
    display_.refresh();
}

void Ui::showError(const std::string &title, const std::string &message) {
    display_.clear();
    header(false);
    display_.drawText(8, 52, title, 2);
    wrappedText(8, 88, 23, 4, message);
    footer("hold menu");
    display_.refresh();
}

} // namespace display
