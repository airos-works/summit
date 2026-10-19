#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "core/DownloadText.h"
#include <Messenger.h>
#include <OS.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class BMessage;
namespace summit {
// Sent to the list's listeners (browser windows, their downloads panels)
// when a download started, moved on, ended or was taken off the list.
constexpr uint32 kDownloadsChanged = 'dlch';

// The downloads of this session, newest first, for the toolbar's downloads
// button and its panel in every window. The application feeds it the
// engine's B_WEBKIT_DOWNLOAD_* notifications; private windows see only
// their own downloads, which are forgotten with the private session.
class DownloadList {
public:
    struct Entry {
        uint64 identifier = 0;
        std::string url, path, filename;
        DownloadStatus::State state = DownloadStatus::State::Active;
        uint64 received = 0;
        int64 expected = -1;
        std::string error;
        bool privateBrowsing = false;
        // Finished since its window's panel was last open: the button shows a dot.
        bool unseen = false;
        bigtime_t started = 0, finished = 0;
        // Bytes a second, smoothed over the last updates.
        double rate = 0;
        bigtime_t sampleTime = 0;
        uint64 sampleBytes = 0;
        DownloadStatus Status() const;
    };
    struct Summary {
        int32 active = 0;
        // Of the active downloads together, 0 to 1; -1 when a size is unknown.
        float progress = -1;
        // Finished downloads the panel has not shown yet, and whether one failed.
        bool unseen = false, unseenFailure = false;
    };
    static DownloadList& Default();

    // A B_WEBKIT_DOWNLOAD_* notification. A new download takes
    // privateBrowsing; later ones keep what it was.
    void Update(const BMessage& notification, bool privateBrowsing);
    std::vector<Entry> Entries(bool privateBrowsing) const;
    std::optional<Entry> Find(uint64 identifier) const;
    Summary Summarize(bool privateBrowsing) const;
    // A finished download leaves the list (its file stays).
    void Remove(uint64 identifier);
    void ClearFinished(bool privateBrowsing);
    void ForgetPrivate();
    void MarkSeen(bool privateBrowsing);

    void AddListener(const BMessenger&);
    void RemoveListener(const BMessenger&);

private:
    void Announce();
    mutable std::mutex fMutex;
    std::vector<Entry> fEntries;
    std::vector<BMessenger> fListeners;
};
}
#endif
