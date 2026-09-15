#include "storage/note_store.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
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

static bool mkdirIfMissing(const char *path) {
    if (::mkdir(path, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed: %s", path, std::strerror(errno));
    return false;
}

bool NoteStore::init() {
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

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(config::SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ensureDirectories();
}

bool NoteStore::ensureDirectories() {
    return mkdirIfMissing("/sdcard/audio") &&
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

bool NoteStore::commitRecording(const std::string &id) {
    const std::string src = recordingTempPath();
    const std::string dst = pendingAudioPath(id);
    ::unlink(dst.c_str());
    if (::rename(src.c_str(), dst.c_str()) != 0) {
        ESP_LOGE(TAG, "rename recording failed: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool NoteStore::discardRecording() {
    const std::string path = recordingTempPath();
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::vector<std::string> NoteStore::pendingIds() const {
    std::vector<std::string> ids;
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
            ids.push_back(name.substr(0, name.size() - 4));
        }
    }
    ::closedir(dir);
    std::sort(ids.begin(), ids.end());
    return ids;
}

size_t NoteStore::pendingCount() const {
    return pendingIds().size();
}

bool NoteStore::writeTranscript(
    const std::string &id,
    const std::string &text,
    const std::string &language,
    double duration,
    const std::string &model
) {
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

bool NoteStore::archiveAudio(const std::string &id) {
    const std::string src = pendingAudioPath(id);
    const std::string dst = archivedAudioPath(id);
    ::unlink(dst.c_str());
    return ::rename(src.c_str(), dst.c_str()) == 0;
}

} // namespace storage
