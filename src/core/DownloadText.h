#pragma once
#include <cstdint>
#include <string>

namespace summit {
// What the downloads panel says about a download (the second line of its
// row), worded as Firefox's panel words it.
struct DownloadStatus {
    enum class State { Active, Succeeded, Failed, Cancelled };
    State state = State::Active;
    uint64_t received = 0;
    // -1 when the server did not say.
    int64_t expected = -1;
    // Bytes a second lately; 0 while not known.
    double rate = 0;
    // The site it came from ("example.com").
    std::string host;
    std::string error;
    // A finished download's file is no longer where it was saved.
    bool missing = false;
};
// "512 bytes", "12.4 KiB", "3.1 MiB", "1.25 GiB".
std::string FormatByteSize(uint64_t bytes);
// "3.1 of 15.2 MiB": both in the unit of total.
std::string FormatByteProgress(uint64_t received, uint64_t total);
// "5 seconds left", "2 minutes left", "1 hour 5 minutes left".
std::string FormatTimeLeft(double seconds);
std::string DownloadStatusText(const DownloadStatus&);
// How much of the download has arrived, 0 to 1; -1 when the size is unknown.
double DownloadFraction(uint64_t received, int64_t expected);
}
