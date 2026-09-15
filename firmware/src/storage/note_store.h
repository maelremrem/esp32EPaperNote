#pragma once

#include <string>
#include <vector>

namespace storage {

class NoteStore {
public:
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
