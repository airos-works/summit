#include "BrowserProfiles.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace summit {
using nlohmann::json;
namespace {
constexpr const char* kFile = "profiles.json";
constexpr const char* kFolder = "Profiles";
constexpr size_t kMaximumProfiles = 64;

BrowserProfile DefaultEntry() { return {BrowserProfiles::kDefaultID, "Personal", "blue", false}; }
}

const std::vector<ProfileColor>& ProfileColors()
{
    static const std::vector<ProfileColor> colors {
        {"blue", "Blue", 26, 115, 232}, {"red", "Red", 217, 48, 37}, {"green", "Green", 30, 142, 62},
        {"purple", "Purple", 161, 66, 244}, {"orange", "Orange", 250, 144, 62}, {"pink", "Pink", 208, 24, 132},
        {"cyan", "Cyan", 0, 123, 131}, {"yellow", "Yellow", 249, 171, 0}, {"grey", "Grey", 95, 99, 104},
    };
    return colors;
}

const ProfileColor& FindProfileColor(const std::string& name)
{
    for (const auto& color : ProfileColors()) if (name == color.name) return color;
    return ProfileColors().front();
}

bool BrowserProfiles::ValidID(const std::string& id)
{
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    });
}

std::string BrowserProfiles::CleanName(const std::string& name)
{
    std::string result;
    for (unsigned char c : name) result += c < 0x20 || c == 0x7f ? ' ' : char(c);
    const auto first = result.find_first_not_of(' ');
    if (first == std::string::npos) return { };
    result = result.substr(first, result.find_last_not_of(' ') - first + 1);
    if (result.size() > 40) {
        size_t end = 40;
        // Not in the middle of a UTF-8 sequence.
        while (end > 0 && (static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
        result.resize(end);
        while (!result.empty() && result.back() == ' ') result.pop_back();
    }
    return result;
}

std::filesystem::path BrowserProfiles::Directory(const std::filesystem::path& base, const std::string& id)
{
    return id == kDefaultID ? base : base / kFolder / id;
}

std::pair<std::filesystem::path, std::string> BrowserProfiles::Locate(const std::filesystem::path& directory)
{
    // "…/Profiles/work/" names the same folder.
    const auto folder = directory.has_filename() ? directory : directory.parent_path();
    const std::string id = folder.filename().string();
    const auto parent = folder.parent_path();
    if (parent.filename() == kFolder && ValidID(id) && id != kDefaultID) return {parent.parent_path(), id};
    return {directory, kDefaultID};
}

BrowserProfiles BrowserProfiles::Load(const std::filesystem::path& base)
{
    BrowserProfiles result;
    std::ifstream input(base / kFile, std::ios::binary);
    if (input) {
        try {
            const json document = json::parse(input);
            const auto& list = document.at("profiles");
            std::set<std::string> seen;
            for (const auto& item : list) {
                if (!item.is_object() || result.entries.size() >= kMaximumProfiles) continue;
                BrowserProfile entry;
                entry.id = item.value("id", std::string());
                entry.name = CleanName(item.value("name", std::string()));
                entry.color = FindProfileColor(item.value("color", std::string())).name;
                entry.reopen = item.value("reopen", false);
                if (!ValidID(entry.id) || !seen.insert(entry.id).second) continue;
                if (entry.name.empty()) entry.name = entry.id == kDefaultID ? "Personal" : entry.id;
                result.entries.push_back(std::move(entry));
            }
        } catch (const std::exception&) {
            result.entries.clear();
        }
    }
    auto found = std::find_if(result.entries.begin(), result.entries.end(), [](const auto& entry) { return entry.id == kDefaultID; });
    if (found == result.entries.end()) result.entries.insert(result.entries.begin(), DefaultEntry());
    else std::rotate(result.entries.begin(), found, found + 1);
    result.entries.front().reopen = false;
    return result;
}

bool BrowserProfiles::Save(const std::filesystem::path& base, std::string& error) const
{
    json list = json::array();
    for (const auto& entry : entries) {
        json item = {{"id", entry.id}, {"name", entry.name}, {"color", entry.color}};
        if (entry.reopen) item["reopen"] = true;
        list.push_back(std::move(item));
    }
    const std::string data = json({{"version", 1}, {"profiles", list}}).dump(2) + "\n";
    const auto path = base / kFile;
    const auto temporary = base / (std::string(kFile) + ".new");
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    bool written = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()) && ::fsync(fd) == 0;
    if (!written) error = std::strerror(errno);
    ::close(fd);
    if (written && ::rename(temporary.c_str(), path.c_str()) != 0) {
        error = std::strerror(errno);
        written = false;
    }
    if (!written) ::unlink(temporary.c_str());
    return written;
}

bool BrowserProfiles::Update(const std::filesystem::path& base, const std::function<bool(BrowserProfiles&)>& change,
    std::string& error)
{
    std::error_code ignored;
    std::filesystem::create_directories(base, ignored);
    const int lock = ::open((base / "profiles.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock >= 0) while (::flock(lock, LOCK_EX) != 0 && errno == EINTR) { }
    auto profiles = Load(base);
    bool result = true;
    if (change(profiles)) result = profiles.Save(base, error);
    if (lock >= 0) ::close(lock);
    return result;
}

const BrowserProfile* BrowserProfiles::Find(const std::string& id) const
{
    for (const auto& entry : entries) if (entry.id == id) return &entry;
    return nullptr;
}

BrowserProfile* BrowserProfiles::Find(const std::string& id)
{
    for (auto& entry : entries) if (entry.id == id) return &entry;
    return nullptr;
}

const BrowserProfile& BrowserProfiles::Add(const std::string& name, const std::string& color)
{
    std::string stem;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 0x80) stem += char(std::tolower(c));
        if (stem.size() >= 24) break;
    }
    if (stem.empty() || stem == kDefaultID) stem = "profile";
    std::string id = stem;
    for (int number = 2; Find(id); ++number) id = stem + std::to_string(number);
    BrowserProfile entry {id, CleanName(name), FindProfileColor(color).name, false};
    if (entry.name.empty()) entry.name = "Profile";
    entries.push_back(std::move(entry));
    return entries.back();
}

bool BrowserProfiles::Remove(const std::string& id)
{
    if (id == kDefaultID) return false;
    const auto before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.id == id; }), entries.end());
    return entries.size() != before;
}

std::string BrowserProfiles::UnusedColor() const
{
    for (const auto& color : ProfileColors())
        if (std::none_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.color == color.name; }))
            return color.name;
    return ProfileColors()[entries.size() % ProfileColors().size()].name;
}
}
