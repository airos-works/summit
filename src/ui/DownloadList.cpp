#include "DownloadList.h"
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <Message.h>
#include <algorithm>

namespace summit {
namespace {
// Finished downloads kept on the list; the oldest go first.
constexpr size_t kKeptFinished = 40;

std::string HostOf(const std::string& url)
{
    const auto start = url.find("://");
    if (start == std::string::npos) return {};
    auto host = url.substr(start + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
    if (host.compare(0, 4, "www.") == 0) host.erase(0, 4);
    return host;
}
}

DownloadStatus DownloadList::Entry::Status() const
{
    DownloadStatus status;
    status.state = state;
    status.received = received;
    status.expected = expected;
    status.rate = rate;
    status.host = HostOf(url);
    status.error = error;
    return status;
}

DownloadList& DownloadList::Default()
{
    static DownloadList list;
    return list;
}

void DownloadList::Update(const BMessage& message, bool privateBrowsing)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (!identifier) return;
    {
        std::lock_guard lock(fMutex);
        auto entry = std::find_if(fEntries.begin(), fEntries.end(), [&](const Entry& item) { return item.identifier == identifier; });
        if (message.what == B_WEBKIT_DOWNLOAD_STARTED && entry == fEntries.end()) {
            Entry item;
            item.identifier = identifier;
            item.privateBrowsing = privateBrowsing;
            item.started = system_time();
            fEntries.insert(fEntries.begin(), item);
            entry = fEntries.begin();
        }
        // Updates for downloads that were taken off the list (a private
        // session that ended) are not news.
        if (entry == fEntries.end() || entry->state != DownloadStatus::State::Active) return;
        if (const char* url = message.GetString("url", nullptr); url && *url) entry->url = url;
        if (const char* path = message.GetString("path", nullptr); path && *path) entry->path = path;
        if (const char* filename = message.GetString("filename", nullptr); filename && *filename) entry->filename = filename;
        const bigtime_t now = system_time();
        const uint64 received = message.GetUInt64("current_size", entry->received);
        entry->expected = message.GetInt64("expected_size", entry->expected);
        if (!entry->sampleTime) {
            entry->sampleTime = now;
            entry->sampleBytes = received;
        } else if (now - entry->sampleTime >= 500000 && received >= entry->sampleBytes) {
            const double rate = (received - entry->sampleBytes) * 1e6 / double(now - entry->sampleTime);
            entry->rate = entry->rate > 0 ? entry->rate * 0.6 + rate * 0.4 : rate;
            entry->sampleTime = now;
            entry->sampleBytes = received;
        }
        entry->received = received;
        if (message.what == B_WEBKIT_DOWNLOAD_FINISHED) {
            const uint32 result = message.GetUInt32("result", B_WEBKIT_DOWNLOAD_FAILED);
            entry->state = result == B_WEBKIT_DOWNLOAD_SUCCEEDED ? DownloadStatus::State::Succeeded
                : result == B_WEBKIT_DOWNLOAD_CANCELLED ? DownloadStatus::State::Cancelled : DownloadStatus::State::Failed;
            entry->error = message.GetString("error_description", "");
            entry->finished = now;
            entry->rate = 0;
            entry->unseen = entry->state != DownloadStatus::State::Cancelled;
            size_t finished = 0;
            std::erase_if(fEntries, [&](const Entry& item) {
                return item.state != DownloadStatus::State::Active && ++finished > kKeptFinished;
            });
        }
    }
    Announce();
}

std::vector<DownloadList::Entry> DownloadList::Entries(bool privateBrowsing) const
{
    std::lock_guard lock(fMutex);
    std::vector<Entry> entries;
    for (const auto& entry : fEntries)
        if (entry.privateBrowsing == privateBrowsing) entries.push_back(entry);
    return entries;
}

std::optional<DownloadList::Entry> DownloadList::Find(uint64 identifier) const
{
    std::lock_guard lock(fMutex);
    for (const auto& entry : fEntries)
        if (entry.identifier == identifier) return entry;
    return std::nullopt;
}

DownloadList::Summary DownloadList::Summarize(bool privateBrowsing) const
{
    std::lock_guard lock(fMutex);
    Summary summary;
    uint64 received = 0, expected = 0;
    bool sized = true;
    for (const auto& entry : fEntries) {
        if (entry.privateBrowsing != privateBrowsing) continue;
        if (entry.state == DownloadStatus::State::Active) {
            ++summary.active;
            received += entry.received;
            if (entry.expected > 0) expected += uint64(entry.expected);
            else sized = false;
        } else if (entry.unseen) {
            summary.unseen = true;
            if (entry.state == DownloadStatus::State::Failed) summary.unseenFailure = true;
        }
    }
    if (summary.active && sized && expected) summary.progress = float(DownloadFraction(received, int64(expected)));
    return summary;
}

void DownloadList::Remove(uint64 identifier)
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [&](const Entry& entry) {
            return entry.identifier == identifier && entry.state != DownloadStatus::State::Active;
        });
    }
    Announce();
}

void DownloadList::ClearFinished(bool privateBrowsing)
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [&](const Entry& entry) {
            return entry.privateBrowsing == privateBrowsing && entry.state != DownloadStatus::State::Active;
        });
    }
    Announce();
}

void DownloadList::ForgetPrivate()
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [](const Entry& entry) { return entry.privateBrowsing; });
    }
    Announce();
}

void DownloadList::MarkSeen(bool privateBrowsing)
{
    bool changed = false;
    {
        std::lock_guard lock(fMutex);
        for (auto& entry : fEntries)
            if (entry.privateBrowsing == privateBrowsing && entry.unseen) {
                entry.unseen = false;
                changed = true;
            }
    }
    if (changed) Announce();
}

void DownloadList::AddListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    fListeners.push_back(listener);
}

void DownloadList::RemoveListener(const BMessenger& listener)
{
    std::lock_guard lock(fMutex);
    std::erase(fListeners, listener);
}

void DownloadList::Announce()
{
    std::vector<BMessenger> listeners;
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fListeners, [](const BMessenger& listener) { return !listener.IsValid(); });
        listeners = fListeners;
    }
    // Never wait for a busy window: a later notice says the same.
    BMessage changed(kDownloadsChanged);
    for (const auto& listener : listeners) listener.SendMessage(&changed, static_cast<BHandler*>(nullptr), 0);
}
}
#endif
