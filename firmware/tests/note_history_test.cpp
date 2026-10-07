#include "storage/note_store.h"
#include "project_config.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(1); } } while (0)

static storage::NoteStore store;
static void reset() {
    fs::remove_all(config::SD_MOUNT_POINT);
    for (const char *path : {config::PENDING_DIR, config::ARCHIVE_DIR, config::NOTES_DIR})
        fs::create_directories(path);
}
static void put(const std::string &path, const std::string &body = "wav") {
    std::ofstream file(path, std::ios::binary);
    file << body;
    CHECK(file.good());
}
static void union_deduplicated_descending() {
    reset();
    put(store.pendingAudioPath("20261006T120000Z-0001"));
    CHECK(store.writeTranscript("20261006T120000Z-0001", "Saved text", "en", 3.0, "whistle"));
    put(store.archivedAudioPath("boot-9000-0002"));
    CHECK(store.writeTranscript("20261005T120000Z-0001", "MD only", "en", 1.0, "whistle"));
    const auto notes = store.savedNotes();
    CHECK(notes.size() == 3);
    CHECK(notes[0].id == "boot-9000-0002" && !notes[0].transcribed);
    CHECK(notes[1].id == "20261006T120000Z-0001" && notes[1].transcribed);
    CHECK(notes[2].id == "20261005T120000Z-0001" && notes[2].transcribed);
}
static void newest_limits() {
    reset();
    for (int i = 129; i >= 0; --i) {
        char id[16];
        std::snprintf(id, sizeof(id), "note-%03d", i);
        put(store.pendingAudioPath(id));
    }
    CHECK(store.writeTranscript("note-129", "newest", "en", 1.0, "whistle"));
    CHECK(store.savedNotes(0).empty());
    auto notes = store.savedNotes(1);
    CHECK(notes.size() == 1 && notes[0].id == "note-129" && notes[0].transcribed);
    notes = store.savedNotes();
    CHECK(notes.size() == 100 && notes.back().id == "note-030");
    CHECK(store.savedNotes(200).size() == 130);
    CHECK(store.savedNotes(static_cast<size_t>(-1)).size() == 130);
    put(store.archivedAudioPath("z-newest"));
    CHECK(store.writeTranscript("z-newest", "new winner", "en", 1.0, "whistle"));
    notes = store.savedNotes(1);
    CHECK(notes.size() == 1 && notes[0].id == "z-newest" && notes[0].transcribed);
}
static void safe_regular_entries_only() {
    reset();
    put(store.pendingAudioPath("Good_ID-123"));
    for (const std::string &id : {std::string(".hidden"), std::string("bad name"),
             std::string("bad\\name"), std::string("bad%2fid"), std::string("bad.name"),
             std::string("bad\nname"), std::string(97, 'z')})
        put(store.pendingAudioPath(id));
    put(std::string(config::PENDING_DIR) + "/.wav");
    put(std::string(config::NOTES_DIR) + "/note.md.tmp");
    put(std::string(config::NOTES_DIR) + "/index.jsonl");
    fs::create_directory(store.pendingAudioPath("directory"));
    fs::create_directory(store.markdownPath("Good_ID-123"));
    fs::create_symlink(store.pendingAudioPath("Good_ID-123"), store.archivedAudioPath("linked"));
    CHECK(::mkfifo(store.pendingAudioPath("pipe").c_str(), 0600) == 0);
    const auto notes = store.savedNotes();
    CHECK(notes.size() == 1);
    CHECK(notes[0].id == "Good_ID-123" && !notes[0].transcribed);
}
static void generated_transcript_roundtrip() {
    reset();
    const std::string expected = "  Meeting notes\n\n---\n\nKeep this separator.\nTrailing space  \n";
    CHECK(store.writeTranscript("roundtrip", expected, "en", 42.0, "whistle"));
    std::string actual = "old text";
    CHECK(store.readTranscript("roundtrip", actual));
    CHECK(actual == expected);
    CHECK(store.writeTranscript("empty", "", "en", 0.0, "whistle"));
    CHECK(store.readTranscript("empty", actual) && actual.empty());
}
static void bounded_transcript_preserves_embedded_footer() {
    reset();
    const std::string embedded = "First\n\n---\n\n- ID: long\n- Duration: 1.00 s\n- Language: en\n- STT: whistle\n- Status: synced\nStill transcript\n";
    std::string expected = embedded;
    for (size_t i = 0; i < 9000; ++i) expected += "é";
    CHECK(store.writeTranscript("long", expected, "en", 3.0, "whistle"));
    std::string actual;
    CHECK(store.readTranscript("long", actual));
    const std::string marker = "\n\n[Transcript truncated]";
    CHECK(actual.size() <= 16 * 1024);
    CHECK(actual.size() >= marker.size());
    CHECK(actual.compare(actual.size() - marker.size(), marker.size(), marker) == 0);
    const std::string prefix = actual.substr(0, actual.size() - marker.size());
    CHECK(prefix.size() > 14000 && prefix.size() < expected.size());
    CHECK(expected.compare(0, prefix.size(), prefix) == 0);
    CHECK((prefix.size() - embedded.size()) % 2 == 0);
    CHECK(store.writeTranscript("long", embedded + "end", "en", 3.0, "whistle"));
    CHECK(store.readTranscript("long", actual) && actual == embedded + "end");
}
static void unreadable_missing_unsafe_and_malformed() {
    reset();
    std::string actual;
    const auto rejects = [&](const std::string &id) {
        actual = "stale";
        CHECK(!store.readTranscript(id, actual));
        CHECK(actual.empty());
    };
    for (const auto &id : {std::string(""), std::string("../escape"), std::string("/absolute"),
             std::string("bad\\id"), std::string("bad%2fid"), std::string("bad.id"),
             std::string("bad\0id", 6), std::string(97, 'x'), std::string("missing")}) rejects(id);
    fs::create_directory(store.markdownPath("directory"));
    CHECK(::mkfifo(store.markdownPath("pipe").c_str(), 0600) == 0);
    CHECK(store.writeTranscript("valid", "text", "en", 1.0, "whistle"));
    fs::create_symlink(store.markdownPath("valid"), store.markdownPath("linked"));
    rejects("directory");
    rejects("pipe");
    rejects("linked");
    CHECK(::geteuid() != 0); // Permission tests must run without root bypass.
    CHECK(::chmod(store.markdownPath("valid").c_str(), 0000) == 0);
    rejects("valid");
    CHECK(::chmod(store.markdownPath("valid").c_str(), 0600) == 0);
    CHECK(::chmod(config::NOTES_DIR, 0000) == 0);
    rejects("valid");
    CHECK(store.savedNotes().empty());
    CHECK(::chmod(config::NOTES_DIR, 0700) == 0);
    fs::remove_all(config::SD_MOUNT_POINT);
    CHECK(store.savedNotes().empty());
    rejects("valid");
    reset();
    for (const std::string &body : {std::string(), std::string("plain text"),
             std::string("# Note malformed\n\ntext"),
             std::string("# Note other\n\ntext\n\n---\n\n- ID: malformed\n"),
             std::string("# Note malformed\n\ntext\n\n---\n\n- ID: other\n"),
             std::string("# Note malformed\n\ntext\n\n---\n\n- ID: malformed\n")}) {
        put(store.markdownPath("malformed"), body);
        rejects("malformed");
    }
}
static void byte_boundaries_and_bad_large_footer() {
    reset();
    std::string actual;
    for (size_t size : {size_t(15000), size_t(16000), size_t(16384), size_t(40000)}) {
        const std::string expected(size, 'a');
        CHECK(store.writeTranscript("boundary", expected, "en", 1.0, "whistle"));
        CHECK(store.readTranscript("boundary", actual));
        if (size <= 16000) CHECK(actual == expected);
        else {
            CHECK(actual.size() <= 16384);
            CHECK(actual.find("[Transcript truncated]") != std::string::npos);
        }
    }
    put(store.markdownPath("boundary"), "# Note boundary\n\n" + std::string(40000, 'x'));
    CHECK(!store.readTranscript("boundary", actual) && actual.empty());
    CHECK(store.writeTranscript("boundary", std::string(20000, 'x'), "en", 1.0, std::string(2000, 'm')));
    CHECK(!store.readTranscript("boundary", actual) && actual.empty());
}
int main() {
    union_deduplicated_descending();
    std::cout << "PASS union_deduplicated_descending\n";
    newest_limits();
    std::cout << "PASS newest_limits\n";
    safe_regular_entries_only();
    std::cout << "PASS safe_regular_entries_only\n";
    generated_transcript_roundtrip();
    std::cout << "PASS generated_transcript_roundtrip\n";
    bounded_transcript_preserves_embedded_footer();
    std::cout << "PASS bounded_transcript_preserves_embedded_footer\n";
    unreadable_missing_unsafe_and_malformed();
    std::cout << "PASS unreadable_missing_unsafe_and_malformed\n";
    byte_boundaries_and_bad_large_footer();
    std::cout << "PASS byte_boundaries_and_bad_large_footer\n";
}
