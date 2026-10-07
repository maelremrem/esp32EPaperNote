#include "storage/note_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "board_pins.h"
#include "project_config.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

namespace storage {

static const char *TAG = "store";
static sdmmc_card_t *s_card = nullptr;
static bool s_ready = false;
static bool s_cleanup_failed = false;
static std::string s_error;

std::string NoteStore::lastError() const { return s_error; }
Usage NoteStore::usage() const {
    Usage result;
    uint64_t total=0, free=0;
    if (s_ready && s_card && !s_cleanup_failed &&
        esp_vfs_fat_info(config::SD_MOUNT_POINT, &total, &free)==ESP_OK && total && free<=total)
        result={true,total,free};
    return result;
}

static bool mkdirIfMissing(const char *path) {
    if (::mkdir(path, 0775) == 0) return true;
    if (errno == EEXIST) {
        struct stat info{};
        if (::stat(path, &info) == 0 && S_ISDIR(info.st_mode)) return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed: %s", path, std::strerror(errno));
    s_error = std::string("SD directory setup: ") + std::strerror(errno);
    return false;
}

static bool releaseUnusableMount() {
    if (!s_card) return true;
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(config::SD_MOUNT_POINT, s_card);
    // IDF may free the card before reporting a VFS-unregister error. Never
    // reuse that pointer or mount over uncertain VFS state; require reboot.
    s_card = nullptr;
    if (err != ESP_OK) {
        s_cleanup_failed = true;
        s_error = std::string("SD cleanup: ") + esp_err_to_name(err) + "; restart required";
        ESP_LOGE(TAG, "SD cleanup failed; reboot required: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool NoteStore::init() {
    if (s_ready) return true;
    if (s_cleanup_failed) return false;
    if (!releaseUnusableMount()) return false;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = board::SD_CLK;
    slot.cmd = board::SD_CMD;
    slot.d0 = board::SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false;
    mount.max_files = 8;
    mount.allocation_unit_size = 16 * 1024;

    // Treat the output as valid only on success: late IDF failures can leave
    // an output pointer to a card that its cleanup has already freed.
    sdmmc_card_t *mounted_card = nullptr;
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(config::SD_MOUNT_POINT, &host, &slot, &mount, &mounted_card);
    if (err != ESP_OK) {
        s_error = std::string("SD mount: ") + esp_err_to_name(err) +
            "; SDMMC 1-bit CLK39 CMD41 D0=40. Check FAT32 and card contact.";
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }

    s_card = mounted_card;
    sdmmc_card_print_info(stdout, s_card);
    s_ready = ensureDirectories();
    if (!s_ready) releaseUnusableMount();
    else s_error.clear();
    return s_ready;
}

bool NoteStore::format() {
    if (!s_ready || !s_card || s_cleanup_failed) return false;
    s_ready = false;
    const esp_err_t probe = sdmmc_get_status(s_card);
    const esp_err_t formatted = probe == ESP_OK
        ? esp_vfs_fat_sdcard_format(config::SD_MOUNT_POINT, s_card) : probe;
    if (formatted != ESP_OK) {
        s_error = std::string(probe == ESP_OK ? "SD format: " : "SD card status: ") + esp_err_to_name(formatted);
        releaseUnusableMount();
        return false;
    }
    s_ready = ensureDirectories();
    if (!s_ready) releaseUnusableMount();
    else s_error.clear();
    return s_ready;
}

bool NoteStore::ensureDirectories() {
    return mkdirIfMissing((std::string(config::SD_MOUNT_POINT) + "/audio").c_str()) &&
           mkdirIfMissing(config::RECORDING_DIR) &&
           mkdirIfMissing(config::PENDING_DIR) &&
           mkdirIfMissing(config::ARCHIVE_DIR) &&
           mkdirIfMissing(config::NOTES_DIR);
}

std::string NoteStore::makeNoteId() {
    ++sequence_;
    const std::time_t now = std::time(nullptr);
    char id[config::MAX_NOTE_ID]{};

    if (now > 1704067200) {
        std::tm tm{};
        gmtime_r(&now, &tm);
        std::snprintf(
            id,
            sizeof(id),
            "%04d%02d%02dT%02d%02d%02dZ-%04lu",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            static_cast<unsigned long>(sequence_ % 10000)
        );
    } else {
        const unsigned long long boot_us = static_cast<unsigned long long>(esp_timer_get_time());
        std::snprintf(id, sizeof(id), "boot-%llu-%04lu", boot_us, static_cast<unsigned long>(sequence_ % 10000));
    }
    return id;
}

std::string NoteStore::recordingTempPath() const {
    return std::string(config::RECORDING_DIR) + "/current.tmp";
}

std::string NoteStore::pendingAudioPath(const std::string &id) const {
    return std::string(config::PENDING_DIR) + "/" + id + ".wav";
}

std::string NoteStore::archivedAudioPath(const std::string &id) const {
    return std::string(config::ARCHIVE_DIR) + "/" + id + ".wav";
}

std::string NoteStore::markdownPath(const std::string &id) const {
    return std::string(config::NOTES_DIR) + "/" + id + ".md";
}

static bool validHistoryId(const std::string &id);
static bool regularHistoryFile(const std::string &path);
static bool absentFile(const std::string &path) {
    struct stat existing{};
#ifdef ESP_PLATFORM
    return ::stat(path.c_str(),&existing)!=0 && errno==ENOENT;
#else
    return ::lstat(path.c_str(),&existing)!=0 && errno==ENOENT;
#endif
}

bool NoteStore::commitRecording(const std::string &id) {
    if (!s_ready || !validHistoryId(id)) return false;
    const std::string src = recordingTempPath();
    const std::string dst = pendingAudioPath(id);
    if (!absentFile(dst) || !absentFile(archivedAudioPath(id))) {
        s_error="Recording ID collision; temporary WAV retained."; return false;
    }
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        ESP_LOGE(TAG, "rename recording failed: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool NoteStore::discardRecording() {
    if (!s_ready) return false;
    const std::string path = recordingTempPath();
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::vector<std::string> NoteStore::pendingIds() const {
    std::vector<std::string> ids;
    if (!s_ready) return ids;
    DIR *dir = ::opendir(config::PENDING_DIR);
    if (!dir) {
        return ids;
    }

    while (dirent *entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string name(entry->d_name);
        constexpr const char *suffix = ".wav";
        if (name.size() > 4 && name.compare(name.size() - 4, 4, suffix) == 0) {
            const std::string id = name.substr(0, name.size() - 4);
            if (validHistoryId(id) && regularHistoryFile(pendingAudioPath(id))) ids.push_back(id);
        }
    }
    ::closedir(dir);
    std::sort(ids.begin(), ids.end());
    return ids;
}

size_t NoteStore::pendingCount() const {
    return pendingIds().size();
}

static bool validHistoryId(const std::string &id) {
    if (id.empty() || id.size() > config::MAX_NOTE_ID) return false;
    for (const unsigned char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool regularHistoryFile(const std::string &path) {
    struct stat info{};
    // FAT has no symlinks; host tests additionally reject symlinks rather than
    // following an otherwise safe filename outside the fixture/notes directory.
#ifdef ESP_PLATFORM
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
#else
    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
#endif
}

std::vector<SavedNote> NoteStore::savedNotes(size_t limit) const {
    std::vector<SavedNote> notes;
    if (!s_ready || !limit) return notes;
    const char *directories[] = {config::PENDING_DIR, config::ARCHIVE_DIR, config::NOTES_DIR};
    for (size_t source = 0; source < 3; ++source) {
        DIR *dir = ::opendir(directories[source]);
        if (!dir) continue;
        const std::string suffix = source == 2 ? ".md" : ".wav";
        while (dirent *entry = ::readdir(dir)) {
            const std::string name(entry->d_name);
            if (name.size() <= suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            const std::string id = name.substr(0, name.size() - suffix.size());
            if (!validHistoryId(id) ||
                !regularHistoryFile(std::string(directories[source]) + "/" + name)) continue;
            auto at = std::lower_bound(notes.begin(), notes.end(), id,
                [](const SavedNote &note, const std::string &value) { return note.id > value; });
            if (at != notes.end() && at->id == id) {
                at->transcribed = at->transcribed || source == 2;
                at->audio = at->audio || source != 2;
            } else {
                // Keep only the current top N while scanning, never all SD entries.
                const size_t index = static_cast<size_t>(at - notes.begin());
                if (notes.size() == limit) {
                    if (at == notes.end()) continue;
                    notes.pop_back();
                }
                at = notes.begin() + index;
                notes.insert(at, SavedNote{id, source == 2, source != 2});
            }
        }
        ::closedir(dir);
    }
    return notes;
}

bool NoteStore::readTranscript(const std::string &id, std::string &text) const {
    text.clear();
    if (!s_ready || !validHistoryId(id)) return false;
    const std::string path = markdownPath(id);
    if (!regularHistoryFile(path)) return false;
    const auto close_file = [](FILE *opened) { std::fclose(opened); };
    std::unique_ptr<FILE, decltype(close_file)> file(std::fopen(path.c_str(), "rb"), close_file);
    if (!file) return false;

    constexpr size_t max_bytes = 16 * 1024;
    constexpr size_t tail_bytes = 1024;
    if (std::fseek(file.get(), 0, SEEK_END) != 0) return false;
    const long file_size = std::ftell(file.get());
    if (file_size < 0) return false;
    const bool large = static_cast<unsigned long>(file_size) > max_bytes;
    const size_t count = large ? tail_bytes : static_cast<size_t>(file_size);
    const long offset = file_size - static_cast<long>(count);
    const auto read_at = [&](long position, size_t bytes) {
        if (std::fseek(file.get(), position, SEEK_SET) != 0) return false;
        text.resize(bytes);
        return (!bytes || std::fread(&text[0], 1, bytes, file.get()) == bytes) &&
               !std::ferror(file.get());
    };
    if (!read_at(offset, count)) { text.clear(); return false; }
    const std::string heading = "# Note " + id + "\n\n";
    const std::string footer = "\n\n---\n\n- ID: " + id + "\n";
    const size_t end = text.rfind(footer);
    if (end == std::string::npos) { text.clear(); return false; }
    size_t metadata = end + footer.size();
    for (const char *label : {"- Duration: ", "- Language: ", "- STT: "}) {
        if (text.compare(metadata, std::strlen(label), label) != 0) {
            text.clear();
            return false;
        }
        const size_t newline = text.find('\n', metadata);
        if (newline == std::string::npos) { text.clear(); return false; }
        metadata = newline + 1;
    }
    if (text.compare(metadata, std::string::npos, "- Status: synced\n") != 0) {
        text.clear();
        return false;
    }
    const size_t body_end = static_cast<size_t>(offset) + end;
    bool truncated = false;
    if (large) {
        // Read the true footer first: a separator or even an example metadata
        // block inside the transcript must never masquerade as its end.
        const size_t prefix_bytes = std::min(body_end, max_bytes - tail_bytes);
        truncated = prefix_bytes < body_end;
        if (!read_at(0, prefix_bytes)) { text.clear(); return false; }
    } else {
        text.resize(end);
    }
    if (text.compare(0, heading.size(), heading) != 0) {
        text.clear();
        return false;
    }
    text.erase(0, heading.size());
    if (truncated) {
        // Avoid emitting a partial final UTF-8 code point at the byte limit.
        size_t start = text.size() - 1;
        while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
        const unsigned char lead = static_cast<unsigned char>(text[start]);
        const size_t width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                             (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
        if (text.size() - start < width) text.resize(start);
        text += "\n\n[Transcript truncated]";
    }
    return true;
}

bool NoteStore::writeTranscript(
    const std::string &id,
    const std::string &text,
    const std::string &language,
    double duration,
    const std::string &model
) {
    if (!s_ready) return false;
    const std::string final_path = markdownPath(id);
    const std::string temp_path = final_path + ".tmp";

    FILE *file = std::fopen(temp_path.c_str(), "wb");
    if (!file) {
        return false;
    }

    std::fprintf(file, "# Note %s\n\n", id.c_str());
    std::fprintf(file, "%s\n\n---\n\n", text.c_str());
    std::fprintf(file, "- ID: %s\n", id.c_str());
    std::fprintf(file, "- Duration: %.2f s\n", duration);
    std::fprintf(file, "- Language: %s\n", language.c_str());
    std::fprintf(file, "- STT: %s\n", model.c_str());
    std::fprintf(file, "- Status: synced\n");
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);

    ::unlink(final_path.c_str());
    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ::unlink(temp_path.c_str());
        return false;
    }

    FILE *index = std::fopen(config::INDEX_FILE, "ab");
    if (index) {
        std::fprintf(index, "{\"id\":\"%s\",\"file\":\"%s\",\"status\":\"synced\"}\n", id.c_str(), final_path.c_str());
        std::fflush(index);
        fsync(fileno(index));
        std::fclose(index);
    }
    return true;
}

FILE *NoteStore::openDownload(const std::string &id, bool markdown, uint64_t &bytes) const {
    bytes=0;
    if (!s_ready || !validHistoryId(id)) return nullptr;
    std::string path;
    if (markdown) {
        std::string verified;
        if (!readTranscript(id,verified)) return nullptr; // require generated complete metadata
        path=markdownPath(id);
    } else {
        path=pendingAudioPath(id);
        if (!regularHistoryFile(path)) path=archivedAudioPath(id);
    }
    if (!regularHistoryFile(path)) return nullptr;
    int flags=O_RDONLY;
#ifndef ESP_PLATFORM
    flags|=O_NOFOLLOW;
#endif
    const int fd=::open(path.c_str(),flags);
    if (fd<0) return nullptr;
    struct stat info{};
    if (::fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size<0) { ::close(fd); return nullptr; }
    FILE *file=::fdopen(fd,"rb");
    if (!file) { ::close(fd); return nullptr; }
    bytes=static_cast<uint64_t>(info.st_size);
    return file;
}

bool NoteStore::archiveAudio(const std::string &id) { return cancelPending(id); }

bool NoteStore::cancelPending(const std::string &id) {
    if (!s_ready || !validHistoryId(id)) return false;
    const std::string src = pendingAudioPath(id);
    const std::string dst = archivedAudioPath(id);
    if (!regularHistoryFile(src)) return false;
    struct stat existing{};
#ifdef ESP_PLATFORM
    const int found = ::stat(dst.c_str(), &existing);
#else
    const int found = ::lstat(dst.c_str(), &existing);
#endif
    // All SD mutations are main-owned; no concurrent creator can race this check.
    if (found == 0 || errno != ENOENT) {
        s_error = "Archive collision; pending audio retained.";
        return false;
    }
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        s_error = std::string("Cancel/archive failed: ") + std::strerror(errno);
        return false;
    }
    s_error.clear();
    return true;
}

} // namespace storage
