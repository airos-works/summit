#pragma once
#include "DownloadText.h"
#include <cstdint>
#include <string>
#include <vector>

namespace summit {
// A download as the profile keeps it (downloads.json), so that the downloads
// panel still lists it after a restart: where it came from, its file, how far
// it got and how it ended, and what resuming it needs.
struct DownloadRecord {
    // Where the file came from (after redirects), and the address first
    // asked for, which resuming asks again (a redirect's signed address may
    // have expired by then).
    std::string url, requestURL;
    std::string path, filename;
    DownloadStatus::State state = DownloadStatus::State::Active;
    uint64_t received = 0;
    // -1 when the server did not say.
    int64_t expected = -1;
    std::string error;
    // For resuming: the server's ETag or Last-Modified (If-Range), and
    // whether it said it sends parts of files (Accept-Ranges).
    std::string validator;
    bool acceptsRanges = false;
    // An incomplete file is kept at path: the download continues in it.
    bool partial = false;
    // Summit stopped while it downloaded, without pausing it (a crash).
    bool interrupted = false;
    // When it started and ended, in seconds since the epoch.
    int64_t started = 0, finished = 0;
    // Whether resuming continues from where it stopped (otherwise it starts
    // over, as for a server that does not send parts of files).
    bool CanResume() const { return partial && acceptsRanges && received > 0; }
};
// The file's text. Private downloads are never given to it.
std::string EncodeDownloads(const std::vector<DownloadRecord>&);
// What EncodeDownloads wrote, newest first; unusable entries are left out,
// and an unreadable file gives an empty list. A download that was still
// active in the file was cut off (Summit crashed): it comes back paused and
// interrupted, with its partial file if it had one.
std::vector<DownloadRecord> DecodeDownloads(const std::string&);
}
