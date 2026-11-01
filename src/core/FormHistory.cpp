#include "FormHistory.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace summit {
using nlohmann::json;
namespace {
std::string Trimmed(const std::string& text)
{
    const auto begin = text.find_first_not_of(" \t\r\n\f\v");
    if (begin == std::string::npos) return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n\f\v") - begin + 1);
}

// 12-19 digits (spaces and dashes between them) that pass the Luhn check:
// a payment card's number, which is never kept, whatever the field says.
bool LooksLikeCardNumber(const std::string& value)
{
    std::string digits;
    for (unsigned char character : value) {
        if (std::isdigit(character)) digits += static_cast<char>(character);
        else if (character != ' ' && character != '-') return false;
    }
    if (digits.size() < 12 || digits.size() > 19) return false;
    int sum = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
        int digit = digits[digits.size() - 1 - i] - '0';
        if (i % 2) {
            digit *= 2;
            if (digit > 9) digit -= 9;
        }
        sum += digit;
    }
    return sum % 10 == 0;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool OnSite(const std::string& host, const std::string& site)
{
    if (host.empty() || site.empty()) return false;
    return host == site || (host.size() > site.size() && host.compare(host.size() - site.size(), site.size(), site) == 0
        && host[host.size() - site.size() - 1] == '.');
}

// Drops the least recently used values: a field's over kPerField, then any
// over kTotal.
void Evict(std::vector<FormHistoryEntry>& entries, const std::string& field)
{
    auto older = [](const FormHistoryEntry& a, const FormHistoryEntry& b) { return a.last < b.last; };
    while (std::count_if(entries.begin(), entries.end(), [&](const FormHistoryEntry& entry) { return entry.field == field; })
        > static_cast<long>(FormHistory::kPerField)) {
        auto oldest = entries.end();
        for (auto entry = entries.begin(); entry != entries.end(); ++entry)
            if (entry->field == field && (oldest == entries.end() || older(*entry, *oldest))) oldest = entry;
        entries.erase(oldest);
    }
    if (entries.size() > FormHistory::kTotal) {
        std::stable_sort(entries.begin(), entries.end(), older);
        entries.erase(entries.begin(), entries.begin() + (entries.size() - FormHistory::kTotal));
    }
}
}

FormHistory::FormHistory(std::filesystem::path path, bool writable) : fPath(std::move(path)), fWritable(writable) { }

FormHistory::~FormHistory()
{
    {
        std::lock_guard lock(fMutex);
        fStopping = true;
    }
    fSaveWanted.notify_all();
    if (fSaver.joinable()) fSaver.join();
    Save();
}

std::string FormHistory::FieldKey(const std::string& name)
{
    const std::string key = Lower(Trimmed(name));
    return key.size() > kLongestName ? std::string() : key;
}

std::string FormHistory::KeptValue(const std::string& value)
{
    const std::string kept = Trimmed(value);
    if (kept.empty() || kept.size() > kLongest || kept.find_first_of("\r\n") != std::string::npos || LooksLikeCardNumber(kept))
        return {};
    return kept;
}

std::string FormHistory::Encode(const std::vector<FormHistoryEntry>& entries)
{
    json items = json::array();
    for (const auto& entry : entries)
        items.push_back({{"field", entry.field}, {"value", entry.value}, {"host", entry.host},
            {"first", entry.first}, {"last", entry.last}, {"uses", entry.uses}});
    return json({{"version", 1}, {"entries", std::move(items)}}).dump();
}

std::vector<FormHistoryEntry> FormHistory::Decode(const std::string& text)
{
    std::vector<FormHistoryEntry> entries;
    const json document = json::parse(text, nullptr, false);
    if (!document.is_object()) return entries;
    const auto items = document.find("entries");
    if (items == document.end() || !items->is_array()) return entries;
    auto string = [](const json& item, const char* key) {
        const auto found = item.find(key);
        return found != item.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    auto number = [](const json& item, const char* key) -> int64_t {
        const auto found = item.find(key);
        return found != item.end() && found->is_number_integer() ? found->get<int64_t>() : 0;
    };
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        FormHistoryEntry entry { FieldKey(string(item, "field")), KeptValue(string(item, "value")),
            string(item, "host").substr(0, 255), number(item, "first"), number(item, "last"), std::max<int64_t>(1, number(item, "uses")) };
        if (entry.field.empty() || entry.value.empty()) continue;
        if (std::any_of(entries.begin(), entries.end(), [&](const FormHistoryEntry& other) {
                return other.field == entry.field && other.value == entry.value; }))
            continue;
        entries.push_back(std::move(entry));
        if (entries.size() >= kTotal * 2) break;
    }
    // A file from a Summit with larger limits keeps the most recent.
    std::map<std::string, size_t> perField;
    for (const auto& entry : entries) ++perField[entry.field];
    for (const auto& [field, count] : perField)
        if (count > kPerField) Evict(entries, field);
    if (entries.size() > kTotal) Evict(entries, std::string());
    return entries;
}

void FormHistory::Load() const
{
    if (fLoaded) return;
    fLoaded = true;
    std::ifstream file(fPath, std::ios::binary);
    if (!file) return;
    std::stringstream text;
    text << file.rdbuf();
    fEntries = Decode(text.str());
}

void FormHistory::Remember(const std::string& host, const std::vector<std::pair<std::string, std::string>>& values, int64_t now)
{
    bool changed = false;
    {
        std::lock_guard lock(fMutex);
        Load();
        for (const auto& [name, typed] : values) {
            const std::string field = FieldKey(name), value = KeptValue(typed);
            if (field.empty() || value.empty()) continue;
            auto found = std::find_if(fEntries.begin(), fEntries.end(),
                [&](const FormHistoryEntry& entry) { return entry.field == field && entry.value == value; });
            if (found != fEntries.end()) {
                found->last = now;
                found->host = host;
                ++found->uses;
            } else {
                fEntries.push_back({field, value, host, now, now, 1});
                Evict(fEntries, field);
            }
            changed = true;
        }
        if (changed) ++fRevision;
    }
    if (changed) SaveSoon();
}

std::vector<std::string> FormHistory::Values(const std::string& name, const std::string& host) const
{
    const std::string field = FieldKey(name);
    std::vector<const FormHistoryEntry*> found;
    std::lock_guard lock(fMutex);
    Load();
    for (const auto& entry : fEntries) if (entry.field == field) found.push_back(&entry);
    std::stable_sort(found.begin(), found.end(), [&](const FormHistoryEntry* a, const FormHistoryEntry* b) {
        const bool aHere = !host.empty() && a->host == host, bHere = !host.empty() && b->host == host;
        if (aHere != bHere) return aHere;
        if (a->last != b->last) return a->last > b->last;
        return a->uses > b->uses;
    });
    std::vector<std::string> values;
    for (const auto* entry : found) values.push_back(entry->value);
    return values;
}

bool FormHistory::Remove(const std::string& name, const std::string& value)
{
    const std::string field = FieldKey(name);
    {
        std::lock_guard lock(fMutex);
        Load();
        const auto before = fEntries.size();
        fEntries.erase(std::remove_if(fEntries.begin(), fEntries.end(),
            [&](const FormHistoryEntry& entry) { return entry.field == field && entry.value == value; }), fEntries.end());
        if (fEntries.size() == before) return false;
        ++fRevision;
    }
    SaveSoon();
    return true;
}

size_t FormHistory::Clear(int64_t since)
{
    size_t removed;
    {
        std::lock_guard lock(fMutex);
        Load();
        const auto before = fEntries.size();
        fEntries.erase(std::remove_if(fEntries.begin(), fEntries.end(),
            [&](const FormHistoryEntry& entry) { return since <= 0 || entry.last >= since; }), fEntries.end());
        removed = before - fEntries.size();
        // Written even when nothing was in memory: a file that could not be
        // read is replaced by an empty one.
        ++fRevision;
    }
    SaveSoon();
    return removed;
}

size_t FormHistory::ForgetSites(const std::vector<std::string>& sites)
{
    size_t removed;
    {
        std::lock_guard lock(fMutex);
        Load();
        const auto before = fEntries.size();
        fEntries.erase(std::remove_if(fEntries.begin(), fEntries.end(), [&](const FormHistoryEntry& entry) {
            return std::any_of(sites.begin(), sites.end(), [&](const std::string& site) { return OnSite(entry.host, Lower(site)); });
        }), fEntries.end());
        removed = before - fEntries.size();
        if (removed) ++fRevision;
    }
    if (removed) SaveSoon();
    return removed;
}

size_t FormHistory::Count() const
{
    std::lock_guard lock(fMutex);
    Load();
    return fEntries.size();
}

void FormHistory::SaveSoon()
{
    {
        std::lock_guard lock(fMutex);
        if (fStopping || !fWritable) return;
        fSaveRequested = true;
        if (!fSaver.joinable()) fSaver = std::thread([this] { SaveLoop(); });
    }
    fSaveWanted.notify_all();
}

void FormHistory::SaveLoop()
{
    std::unique_lock lock(fMutex);
    while (true) {
        fSaveWanted.wait(lock, [this] { return fSaveRequested || fStopping; });
        if (fStopping) return;
        // A form sends several values, and a list may lose several by
        // Shift+Delete in a row: one write takes them all.
        fSaveWanted.wait_for(lock, std::chrono::milliseconds(500), [this] { return fStopping; });
        if (fStopping) return;
        fSaveRequested = false;
        lock.unlock();
        Save();
        lock.lock();
    }
}

void FormHistory::Save()
{
    std::string data;
    uint64_t revision;
    {
        std::lock_guard lock(fMutex);
        if (!fWritable || !fLoaded || fRevision == fSavedRevision) return;
        data = Encode(fEntries);
        revision = fRevision;
    }
    // A new file beside it, on the disk before it replaces the old one.
    std::string temporary = fPath.string() + ".XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        std::fprintf(stderr, "Summit: could not save the form history: %s\n", std::strerror(errno));
        return;
    }
    size_t written = 0;
    bool failed = false;
    while (written < data.size()) {
        const ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed = true; break; }
        written += count;
    }
    failed = fsync(fd) != 0 || failed;
    failed = close(fd) != 0 || failed;
    if (failed || std::rename(temporary.c_str(), fPath.c_str()) != 0) {
        std::fprintf(stderr, "Summit: could not save the form history: %s\n", std::strerror(errno));
        unlink(temporary.c_str());
        return;
    }
    std::lock_guard lock(fMutex);
    fSavedRevision = std::max(fSavedRevision, revision);
}
}
