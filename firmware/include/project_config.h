#pragma once

#include <cstddef>
#include <cstdint>

namespace config {

constexpr int AUDIO_SAMPLE_RATE = 16000;
constexpr int AUDIO_BITS = 16;
constexpr int AUDIO_CHANNELS = 1;
constexpr int AUDIO_READ_CHUNK = 2048;
constexpr float MIC_GAIN_DB = 36.0f;

constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t BUTTON_DOUBLE_MS = 350;
constexpr uint32_t BUTTON_LONG_MS = 800;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 9000;
constexpr uint32_t WIFI_RETRY_IDLE_MS = 30000;
constexpr uint32_t WIFI_REEVALUATE_MS = 300000;
constexpr uint32_t SYNC_RETRY_MS = 30000;

constexpr uint32_t HTTP_TIMEOUT_MS = 180000;
constexpr size_t HTTP_RESPONSE_MAX = 8192;

constexpr char SD_MOUNT_POINT[] = "/sdcard";
constexpr char RECORDING_DIR[] = "/sdcard/audio/recording";
constexpr char PENDING_DIR[] = "/sdcard/audio/pending";
constexpr char ARCHIVE_DIR[] = "/sdcard/audio/archive";
constexpr char NOTES_DIR[] = "/sdcard/notes";
constexpr char INDEX_FILE[] = "/sdcard/notes/index.jsonl";

constexpr size_t MAX_NOTE_TEXT = 4096;
constexpr size_t MAX_NOTE_ID = 96;

} // namespace config
