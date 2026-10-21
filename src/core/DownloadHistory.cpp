#include "DownloadHistory.h"
#include <nlohmann/json.hpp>
#include <type_traits>

namespace summit {
using nlohmann::json;
namespace {
// More than the panel keeps (DownloadList), for a file written by a later Summit.
constexpr size_t kMaxRecords = 500;
constexpr size_t kMaxText = 65536;

const char* StateName(DownloadStatus::State state)
{
    switch (state) {
    case DownloadStatus::State::Active: return "active";
    case DownloadStatus::State::Paused: return "paused";
    case DownloadStatus::State::Succeeded: return "succeeded";
    case DownloadStatus::State::Failed: return "failed";
    case DownloadStatus::State::Cancelled: return "cancelled";
    }
    return "failed";
}

bool StateNamed(const std::string& name, DownloadStatus::State& state)
{
    for (auto value : {DownloadStatus::State::Active, DownloadStatus::State::Paused, DownloadStatus::State::Succeeded,
             DownloadStatus::State::Failed, DownloadStatus::State::Cancelled}) {
        if (name == StateName(value)) {
            state = value;
            return true;
        }
    }
    return false;
}

std::string Text(const json& item, const char* key)
{
    const auto found = item.find(key);
    if (found == item.end() || !found->is_string()) return {};
    const auto& text = found->get_ref<const std::string&>();
    return text.size() > kMaxText || text.find('\0') != std::string::npos ? std::string() : text;
}

template<typename Number> Number Integer(const json& item, const char* key, Number fallback)
{
    const auto found = item.find(key);
    if (found == item.end() || !found->is_number_integer()) return fallback;
    if (std::is_unsigned<Number>::value && !found->is_number_unsigned()) return fallback;
    return found->get<Number>();
}

bool Flag(const json& item, const char* key)
{
    const auto found = item.find(key);
    return found != item.end() && found->is_boolean() && found->get<bool>();
}
}

std::string EncodeDownloads(const std::vector<DownloadRecord>& records)
{
    json list = json::array();
    for (const auto& record : records) {
        json item = {{"url", record.url}, {"path", record.path}, {"filename", record.filename},
            {"state", StateName(record.state)}, {"received", record.received}, {"expected", record.expected}};
        if (!record.requestURL.empty() && record.requestURL != record.url) item["requestURL"] = record.requestURL;
        if (!record.error.empty()) item["error"] = record.error;
        if (!record.validator.empty()) item["validator"] = record.validator;
        if (record.acceptsRanges) item["acceptsRanges"] = true;
        // An active download's file is there once it has a path.
        if (record.partial || (record.state == DownloadStatus::State::Active && !record.path.empty())) item["partial"] = true;
        if (record.interrupted) item["interrupted"] = true;
        if (record.started) item["started"] = record.started;
        if (record.finished) item["finished"] = record.finished;
        list.push_back(std::move(item));
    }
    return json{{"version", 1}, {"downloads", std::move(list)}}.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
}

std::vector<DownloadRecord> DecodeDownloads(const std::string& text)
{
    std::vector<DownloadRecord> records;
    const json file = json::parse(text, nullptr, false);
    if (!file.is_object() || Integer<int>(file, "version", 0) != 1) return records;
    const auto list = file.find("downloads");
    if (list == file.end() || !list->is_array()) return records;
    for (const auto& item : *list) {
        if (records.size() >= kMaxRecords) break;
        if (!item.is_object()) continue;
        DownloadRecord record;
        if (!StateNamed(Text(item, "state"), record.state)) continue;
        record.url = Text(item, "url");
        record.requestURL = Text(item, "requestURL");
        if (record.requestURL.empty()) record.requestURL = record.url;
        record.path = Text(item, "path");
        record.filename = Text(item, "filename");
        if (record.url.empty() && record.path.empty()) continue;
        record.received = Integer<uint64_t>(item, "received", 0);
        record.expected = Integer<int64_t>(item, "expected", -1);
        record.error = Text(item, "error");
        record.validator = Text(item, "validator");
        record.acceptsRanges = Flag(item, "acceptsRanges");
        record.partial = Flag(item, "partial") && !record.path.empty();
        record.interrupted = Flag(item, "interrupted");
        record.started = Integer<int64_t>(item, "started", 0);
        record.finished = Integer<int64_t>(item, "finished", 0);
        if (record.state == DownloadStatus::State::Active) {
            record.state = DownloadStatus::State::Paused;
            record.interrupted = true;
        }
        records.push_back(std::move(record));
    }
    return records;
}
}
