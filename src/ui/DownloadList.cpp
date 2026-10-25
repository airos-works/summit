#include "DownloadList.h"
#if SUMMIT_MODERN_WEBKIT
#include "core/HistoryVisits.h"
#include <WebKit/WebKitContext.h>
#include <Message.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace summit {
namespace {
// Finished downloads kept on the list; the oldest go first. Paused ones stay.
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

bool InProgress(const DownloadList::Entry& entry)
{
    return entry.state == DownloadStatus::State::Active || entry.state == DownloadStatus::State::Paused;
}

// An incomplete file is of no use once its download is off the list.
void RemovePartialFile(const DownloadList::Entry& entry)
{
    if (entry.partial && !entry.path.empty() && entry.state != DownloadStatus::State::Succeeded) unlink(entry.path.c_str());
}

// A partial file that has gone is no longer one; a paused download continues
// from the size of the file there is.
void CheckPartialFile(DownloadList::Entry& entry)
{
    if (!entry.partial) return;
    struct stat info;
    if (entry.path.empty() || lstat(entry.path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        entry.partial = false;
        return;
    }
    if (entry.state == DownloadStatus::State::Paused || entry.received > uint64(info.st_size)) entry.received = info.st_size;
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
    status.interrupted = interrupted;
    status.resumable = CanResume() || !received;
    status.restarted = restarted;
    return status;
}

bool DownloadList::Entry::CanContinue() const
{
    return state == DownloadStatus::State::Paused || (state == DownloadStatus::State::Failed && partial);
}

DownloadList& DownloadList::Default()
{
    static DownloadList list;
    return list;
}

void DownloadList::Open(const std::filesystem::path& file)
{
    std::vector<DownloadRecord> records;
    {
        std::ifstream stream(file, std::ios::binary);
        std::ostringstream text;
        if (stream) {
            text << stream.rdbuf();
            records = DecodeDownloads(text.str());
        }
    }
    {
        std::lock_guard lock(fMutex);
        fFile = file;
        for (auto& record : records) {
            Entry entry;
            static_cast<DownloadRecord&>(entry) = std::move(record);
            entry.identifier = ++fLastIdentifier;
            CheckPartialFile(entry);
            fEntries.push_back(std::move(entry));
        }
    }
    Announce();
}

std::optional<DownloadList::HistoryChange> DownloadList::Update(const BMessage& message, bool privateBrowsing)
{
    const uint64 engine = message.GetUInt64("identifier", 0);
    if (!engine) return {};
    bool save = false;
    bool persist = false;
    std::optional<HistoryChange> history;
    {
        std::lock_guard lock(fMutex);
        auto entry = std::find_if(fEntries.begin(), fEntries.end(), [&](const Entry& item) { return item.engine == engine; });
        if (const uint64 continues = message.GetUInt64("continues", 0);
            message.what == B_WEBKIT_DOWNLOAD_STARTED && entry == fEntries.end() && continues) {
            // An extension resumed a paused entry (downloads.resume()).
            entry = std::find_if(fEntries.begin(), fEntries.end(), [&](const Entry& item) {
                return item.lastEngine == continues && item.state == DownloadStatus::State::Paused;
            });
            if (entry != fEntries.end()) {
                entry->engine = engine;
                entry->state = DownloadStatus::State::Active;
                entry->error.clear();
                entry->interrupted = entry->partial = entry->restarted = entry->unseen = false;
                entry->rate = 0;
                entry->sampleTime = 0;
                entry->finished = 0;
                save = true;
            }
        }
        if (message.what == B_WEBKIT_DOWNLOAD_STARTED && entry == fEntries.end()) {
            Entry item;
            item.identifier = ++fLastIdentifier;
            item.engine = engine;
            item.privateBrowsing = privateBrowsing;
            item.started = std::time(nullptr);
            fEntries.insert(fEntries.begin(), item);
            entry = fEntries.begin();
            save = true;
        }
        // Updates for downloads that were taken off the list (a private
        // session that ended) are not news.
        if (entry == fEntries.end() || entry->state != DownloadStatus::State::Active) return {};
        persist = !entry->privateBrowsing;
        if (!entry->historyStarted) entry->historyStarted = HistoryNowMicroseconds();
        auto text = [&](const char* key, std::string& field) {
            const char* value = message.GetString(key, nullptr);
            if (value && *value && field != value) {
                field = value;
                save = true;
            }
        };
        text("url", entry->url);
        text("request_url", entry->requestURL);
        text("path", entry->path);
        text("filename", entry->filename);
        if (persist) {
            const auto& url = entry->requestURL.empty() ? entry->url : entry->requestURL;
            if (!entry->historyRecorded && !url.empty()) {
                entry->historyRecorded = true;
                entry->historyTitlePending = entry->filename.empty();
                history = HistoryChange{url, entry->filename, entry->historyStarted, true};
            } else if (entry->historyTitlePending && !entry->filename.empty()) {
                entry->historyTitlePending = false;
                history = HistoryChange{url, entry->filename, entry->historyStarted, false};
            }
        }
        // What resuming needs, once the server has answered.
        if (const char* validator = nullptr; message.FindString("validator", &validator) == B_OK && entry->validator != validator) {
            entry->validator = validator;
            save = true;
        }
        if (bool accepts = false; message.FindBool("accepts_ranges", &accepts) == B_OK && accepts != entry->acceptsRanges) {
            entry->acceptsRanges = accepts;
            save = true;
        }
        if (message.GetBool("restarted", false)) entry->restarted = true;
        const bigtime_t now = system_time();
        const uint64 received = message.GetUInt64("current_size", entry->received);
        entry->expected = message.GetInt64("expected_size", entry->expected);
        if (!entry->sampleTime || received < entry->sampleBytes) {
            entry->sampleTime = now;
            entry->sampleBytes = received;
        } else if (now - entry->sampleTime >= 500000) {
            const double rate = (received - entry->sampleBytes) * 1e6 / double(now - entry->sampleTime);
            entry->rate = entry->rate > 0 ? entry->rate * 0.6 + rate * 0.4 : rate;
            entry->sampleTime = now;
            entry->sampleBytes = received;
        }
        entry->received = received;
        if (message.what == B_WEBKIT_DOWNLOAD_FINISHED) {
            const uint32 result = message.GetUInt32("result", B_WEBKIT_DOWNLOAD_FAILED);
            entry->state = result == B_WEBKIT_DOWNLOAD_SUCCEEDED ? DownloadStatus::State::Succeeded
                : result == B_WEBKIT_DOWNLOAD_PAUSED ? DownloadStatus::State::Paused
                : result == B_WEBKIT_DOWNLOAD_CANCELLED ? DownloadStatus::State::Cancelled : DownloadStatus::State::Failed;
            entry->error = entry->state == DownloadStatus::State::Failed ? message.GetString("error_description", "") : "";
            entry->partial = message.GetBool("partial", false);
            if (entry->state == DownloadStatus::State::Paused && entry->partial && !entry->acceptsRanges) {
                // Its server cannot continue it: resuming starts over, so
                // the partial file is of no use (Summit quitting pauses it).
                unlink(entry->path.c_str());
                entry->partial = false;
            }
            entry->lastEngine = std::exchange(entry->engine, 0);
            entry->finished = std::time(nullptr);
            entry->rate = 0;
            entry->unseen = entry->state == DownloadStatus::State::Succeeded || entry->state == DownloadStatus::State::Failed;
            size_t finished = 0;
            std::erase_if(fEntries, [&](const Entry& item) {
                if (InProgress(item) || ++finished <= kKeptFinished) return false;
                RemovePartialFile(item);
                return true;
            });
            save = true;
        }
    }
    if (save && persist) Save();
    Announce();
    return history;
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

void DownloadList::Resumed(uint64 identifier, uint64 engine)
{
    bool save = false;
    {
        std::lock_guard lock(fMutex);
        for (auto& entry : fEntries) {
            if (entry.identifier != identifier || entry.state == DownloadStatus::State::Active) continue;
            entry.engine = engine;
            entry.state = DownloadStatus::State::Active;
            entry.error.clear();
            entry.interrupted = entry.partial = entry.restarted = entry.unseen = false;
            entry.rate = 0;
            entry.sampleTime = 0;
            entry.finished = 0;
            save = !entry.privateBrowsing;
        }
    }
    if (save) Save();
    Announce();
}

void DownloadList::ResumeFailed(uint64 identifier, const std::string& error)
{
    bool save = false;
    {
        std::lock_guard lock(fMutex);
        for (auto& entry : fEntries) {
            if (entry.identifier != identifier || entry.state == DownloadStatus::State::Active) continue;
            entry.state = DownloadStatus::State::Failed;
            entry.error = error;
            entry.interrupted = false;
            save = !entry.privateBrowsing;
        }
    }
    if (save) Save();
    Announce();
}

void DownloadList::Discard(uint64 identifier)
{
    bool save = false;
    {
        std::lock_guard lock(fMutex);
        for (auto& entry : fEntries) {
            if (entry.identifier != identifier || entry.state != DownloadStatus::State::Paused) continue;
            RemovePartialFile(entry);
            entry.state = DownloadStatus::State::Cancelled;
            entry.partial = entry.interrupted = false;
            entry.finished = std::time(nullptr);
            save = !entry.privateBrowsing;
        }
    }
    if (save) Save();
    Announce();
}

void DownloadList::Remove(uint64 identifier)
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [&](const Entry& entry) {
            if (entry.identifier != identifier || InProgress(entry)) return false;
            RemovePartialFile(entry);
            return true;
        });
    }
    Save();
    Announce();
}

void DownloadList::ClearFinished(bool privateBrowsing)
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [&](const Entry& entry) {
            if (entry.privateBrowsing != privateBrowsing || InProgress(entry)) return false;
            RemovePartialFile(entry);
            return true;
        });
    }
    if (!privateBrowsing) Save();
    Announce();
}

void DownloadList::ForgetPrivate()
{
    {
        std::lock_guard lock(fMutex);
        std::erase_if(fEntries, [](const Entry& entry) {
            if (!entry.privateBrowsing) return false;
            // Nothing of a private session stays behind (its context
            // cancels what still downloads).
            if (entry.state == DownloadStatus::State::Paused || entry.state == DownloadStatus::State::Failed) RemovePartialFile(entry);
            return true;
        });
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

void DownloadList::Save()
{
    // One writer at a time, each with the list as it is then.
    static std::mutex writing;
    std::lock_guard writer(writing);
    std::vector<DownloadRecord> records;
    std::filesystem::path file;
    {
        std::lock_guard lock(fMutex);
        if (fFile.empty()) return;
        file = fFile;
        for (const auto& entry : fEntries)
            if (!entry.privateBrowsing) records.push_back(entry);
    }
    const std::string text = EncodeDownloads(records);
    auto temporary = file;
    temporary += ".new";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.close();
        if (!stream) {
            unlink(temporary.c_str());
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
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
