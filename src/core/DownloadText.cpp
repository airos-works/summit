#include "DownloadText.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace summit {
namespace {
struct Unit {
    double divisor;
    const char* name;
};
Unit UnitFor(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024 * 1024) return { 1024.0 * 1024 * 1024, "GiB" };
    if (bytes >= 1024ull * 1024) return { 1024.0 * 1024, "MiB" };
    if (bytes >= 1024) return { 1024.0, "KiB" };
    return { 1, "bytes" };
}
// One decimal under 100 (two for GiB under 10), none above.
std::string Number(uint64_t bytes, const Unit& unit)
{
    const double value = bytes / unit.divisor;
    char text[32];
    if (unit.divisor == 1) std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(bytes));
    else if (unit.divisor >= 1024.0 * 1024 * 1024 && value < 10) std::snprintf(text, sizeof text, "%.2f", value);
    else if (value < 100) std::snprintf(text, sizeof text, "%.1f", value);
    else std::snprintf(text, sizeof text, "%.0f", value);
    return text;
}
std::string Plural(long count, const char* singular)
{
    return std::to_string(count) + " " + singular + (count == 1 ? "" : "s");
}
}

std::string FormatByteSize(uint64_t bytes)
{
    if (bytes == 1) return "1 byte";
    const auto unit = UnitFor(bytes);
    return Number(bytes, unit) + " " + unit.name;
}

std::string FormatByteProgress(uint64_t received, uint64_t total)
{
    const auto unit = UnitFor(total);
    return Number(received, unit) + " of " + Number(total, unit) + " " + unit.name;
}

std::string FormatTimeLeft(double seconds)
{
    if (!(seconds >= 0) || std::isinf(seconds)) return {};
    const long whole = std::lround(std::ceil(seconds));
    if (whole < 60) return Plural(std::max(whole, 1L), "second") + " left";
    if (whole < 3600) {
        const long minutes = (whole + 30) / 60;
        if (minutes >= 60) return "1 hour left";
        return Plural(minutes, "minute") + " left";
    }
    const long hours = whole / 3600, minutes = (whole % 3600 + 30) / 60;
    if (hours >= 24) return "more than a day left";
    return Plural(hours, "hour") + (minutes ? " " + Plural(minutes, "minute") : "") + " left";
}

double DownloadFraction(uint64_t received, int64_t expected)
{
    if (expected <= 0) return -1;
    return std::min(1.0, double(received) / double(expected));
}

std::string DownloadStatusText(const DownloadStatus& status)
{
    switch (status.state) {
    case DownloadStatus::State::Active: {
        if (!status.received && status.rate <= 0) return "Starting…";
        std::string text = status.expected > 0 && uint64_t(status.expected) >= status.received
            ? FormatByteProgress(status.received, uint64_t(status.expected))
            : FormatByteSize(status.received);
        if (status.rate > 0) {
            text += " — " + FormatByteSize(uint64_t(status.rate)) + "/s";
            if (status.expected > 0 && uint64_t(status.expected) > status.received)
                text += ", " + FormatTimeLeft((uint64_t(status.expected) - status.received) / status.rate);
        }
        return text;
    }
    case DownloadStatus::State::Succeeded:
        if (status.missing) return "File moved or missing";
        return FormatByteSize(status.received) + (status.host.empty() ? "" : " — " + status.host);
    case DownloadStatus::State::Failed:
        return status.error.empty() ? "Failed" : "Failed — " + status.error;
    case DownloadStatus::State::Cancelled:
        return "Cancelled";
    }
    return {};
}
}
