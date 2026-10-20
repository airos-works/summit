#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/DownloadHistory.h"
#include <Messenger.h>
#include <OS.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class BMessage;
namespace summit {
// Sent to the list's listeners (browser windows, their downloads panels)
// when a download started, moved on, ended or was taken off the list.
constexpr uint32 kDownloadsChanged = 'dlch';

// The downloads, newest first, for the toolbar's downloads button and its
// panel in every window. The application feeds it the engine's
// B_WEBKIT_DOWNLOAD_* notifications. The profile keeps the list
// (downloads.json), so that it survives a restart until Clear List, and a
// download that a crash cut off comes back paused, to be resumed. Private
// windows see only their own downloads, which are never written and are
// forgotten (with their partial files) with the private session.
class DownloadList {
public:
    struct Entry : DownloadRecord {
        // The list's own; a download keeps it while it is paused and
        // resumed, each transfer being another engine download.
        uint64 identifier = 0;
        // The engine's download while one runs, else 0.
        uint64 engine = 0;
        bool privateBrowsing = false;
        // Finished since its window's panel was last open: the button shows a dot.
        bool unseen = false;
        // It started over: the server sent the whole file again.
        bool restarted = false;
        // Bytes a second, smoothed over the last updates.
        double rate = 0;
        bigtime_t sampleTime = 0;
        uint64 sampleBytes = 0;
        DownloadStatus Status() const;
        // Paused, or failed with its partial file: Resume (or Restart) continues it.
        bool CanContinue() const;
    };
    struct Summary {
        int32 active = 0;
        // Of the active downloads together, 0 to 1; -1 when a size is unknown.
        float progress = -1;
        // Finished downloads the panel has not shown yet, and whether one failed.
        bool unseen = false, unseenFailure = false;
    };
    static DownloadList& Default();

    // Reads the list the profile keeps in file and writes it there from then on.
    void Open(const std::filesystem::path& file);
    // A B_WEBKIT_DOWNLOAD_* notification. A new download takes
    // privateBrowsing; later ones keep what it was.
    void Update(const BMessage& notification, bool privateBrowsing);
    std::vector<Entry> Entries(bool privateBrowsing) const;
    std::optional<Entry> Find(uint64 identifier) const;
    Summary Summarize(bool privateBrowsing) const;
    // The engine continues the entry as download engine (ResumeDownload).
    void Resumed(uint64 identifier, uint64 engine);
    // It could not: the entry fails with error, keeping its partial file.
    void ResumeFailed(uint64 identifier, const std::string& error);
    // A paused download is given up: cancelled, its partial file removed.
    void Discard(uint64 identifier);
    // A download no longer in progress leaves the list. Its file stays when
    // it is complete; a partial file is removed.
    void Remove(uint64 identifier);
    void ClearFinished(bool privateBrowsing);
    void ForgetPrivate();
    void MarkSeen(bool privateBrowsing);

    void AddListener(const BMessenger&);
    void RemoveListener(const BMessenger&);

private:
    void Announce();
    // Writes the list (but private downloads) to the profile.
    void Save();
    mutable std::mutex fMutex;
    std::vector<Entry> fEntries;
    std::vector<BMessenger> fListeners;
    std::filesystem::path fFile;
    uint64 fLastIdentifier = 0;
};
}
#endif
