#pragma once
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace summit {
// Form history (#149): what the user typed into the ordinary text fields of
// forms they sent (search boxes, names, a comment's author, a city...),
// offered again under fields of the same name. Like Chrome and Firefox it is
// keyed by the field's name (or id) on every site, not per site: a search box
// called "q" or a field called "email" means the same on most sites, and the
// list is the browser's, which a page cannot read; only the value the user
// chooses reaches it. The site a value was last used on is kept too, to list
// that site's values first and to forget them with its data.
//
// Never kept: values longer than kLongest bytes, of several lines, or that
// look like a payment card number. Which fields count (not passwords, not
// autocomplete=off, not card fields) is the page script's part
// (kAutoFillScript); private windows never give values to Remember.
struct FormHistoryEntry {
    // The field's name as FieldKey makes it, and the value as typed.
    std::string field, value;
    // Where it was used last.
    std::string host;
    // First and last use, in seconds since the epoch, and how often.
    int64_t first = 0, last = 0, uses = 0;
};

// Kept in a file of its own in the profile directory (form-history.json), so
// profile.json stays small. All calls may come from any thread; the file is
// read on first use and written on a thread of its own a moment after a
// change (and when the object goes).
class FormHistory {
public:
    static constexpr size_t kPerField = 50, kTotal = 5000, kLongest = 200, kLongestName = 100;
    // writable false: changes are kept for the session only (a newer
    // Summit's profile, which is left alone).
    FormHistory(std::filesystem::path path, bool writable);
    ~FormHistory();
    FormHistory(const FormHistory&) = delete;
    FormHistory& operator=(const FormHistory&) = delete;

    // A sent form's (field name, value) pairs, on host, at now. A field's
    // values over kPerField, and all over kTotal, push out the least
    // recently used.
    void Remember(const std::string& host, const std::vector<std::pair<std::string, std::string>>& values, int64_t now);
    // The field's values, best first: those last used on host, then the
    // most recently used.
    std::vector<std::string> Values(const std::string& field, const std::string& host) const;
    // Shift+Delete on a suggestion.
    bool Remove(const std::string& field, const std::string& value);
    // Forgets the values last used from since on (seconds since the epoch);
    // since 0: all. How many went.
    size_t Clear(int64_t since);
    // Forgets the values last used on these sites (a host or a domain and
    // its subdomains).
    size_t ForgetSites(const std::vector<std::string>& sites);
    size_t Count() const;

    // The key a field's name is kept under: trimmed, lower case; empty when
    // there is none or it is too long.
    static std::string FieldKey(const std::string& name);
    // The value as kept (trimmed), or empty when it is not to be kept.
    static std::string KeptValue(const std::string& value);
    static std::string Encode(const std::vector<FormHistoryEntry>& entries);
    // Unusable entries are left out; an unreadable text gives none.
    static std::vector<FormHistoryEntry> Decode(const std::string& text);

private:
    void Load() const;
    void SaveSoon();
    void SaveLoop();
    void Save();
    const std::filesystem::path fPath;
    const bool fWritable;
    mutable std::mutex fMutex;
    mutable bool fLoaded = false;
    mutable std::vector<FormHistoryEntry> fEntries;
    uint64_t fRevision = 0, fSavedRevision = 0;
    std::thread fSaver;
    std::condition_variable fSaveWanted;
    bool fSaveRequested = false, fStopping = false;
};
}
