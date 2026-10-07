#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace storage {

struct SavedNote {
    std::string id;
    bool transcribed = false;
};

class NoteStore {
public:
    // Main-task-owned, non-destructive SD mount. Idempotent after success;
    // a failed directory setup releases its mount before allowing a retry.
    // A cleanup error requires reboot (IDF may already have freed its card).
    // File operations reject an unavailable mount; paths alone grant no access.
    bool init();
    std::string makeNoteId();

    std::string recordingTempPath() const;
    std::string pendingAudioPath(const std::string &id) const;
    std::string archivedAudioPath(const std::string &id) const;
    std::string markdownPath(const std::string &id) const;

    bool commitRecording(const std::string &id);
    bool discardRecording();
    std::vector<std::string> pendingIds() const;
    size_t pendingCount() const;

    // Regular pending/archive WAVs and note Markdown, deduplicated by safe ID.
    // IDs are ordered lexicographically newest first, not by filesystem time.
    // Only O(limit) entries are kept; transcribed means a regular .md exists.
    std::vector<SavedNote> savedNotes(size_t limit = 100) const;
    // Reads generated Markdown only. False clears text (missing/unreadable or
    // malformed); true may return empty text. At most 16 KiB is read, with a
    // UTF-8-safe prefix + "[Transcript truncated]" when the body exceeds it.
    // Large files require the complete generated metadata in their last 1 KiB.
    bool readTranscript(const std::string &id, std::string &text) const;

    bool writeTranscript(
        const std::string &id,
        const std::string &text,
        const std::string &language,
        double duration,
        const std::string &model
    );
    bool archiveAudio(const std::string &id);

private:
    bool ensureDirectories();
    uint32_t sequence_ = 0;
};

} // namespace storage
