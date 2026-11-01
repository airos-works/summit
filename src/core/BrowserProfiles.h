#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace summit {
// Browser profiles (#157), as in Safari 17: each has its own history,
// bookmarks, settings, extensions, cookies and website data, because each is
// a profile folder of its own (what --profile names). The profile Summit
// always had is the default one; the others are folders in its Profiles
// folder, and its profiles.json lists them all with their names and colours.
// Each profile runs in a Summit process of its own (ProfileSwitcher.h).
struct BrowserProfile {
    // "default" for the default profile; lower-case letters and digits.
    std::string id;
    std::string name;
    // A colour of ProfileColors().
    std::string color;
    // Whether its windows were open when Summit last quit: the default
    // profile opens it again as it restores its own windows.
    bool reopen = false;
    bool operator==(const BrowserProfile&) const = default;
};
struct ProfileColor {
    const char* name;  // as saved
    const char* label; // as shown
    uint8_t red, green, blue;
};
// The tab groups' colours.
const std::vector<ProfileColor>& ProfileColors();
const ProfileColor& FindProfileColor(const std::string& name);

struct BrowserProfiles {
    static constexpr const char* kDefaultID = "default";
    // The default profile first, then the others in the order they were made.
    std::vector<BrowserProfile> entries;

    // base is the default profile's folder. A missing or unreadable file
    // gives the default profile alone; a damaged entry is left out.
    static BrowserProfiles Load(const std::filesystem::path& base);
    // Loads, changes and saves the list under a lock (profiles.lock), so that
    // two profiles' processes changing it at once lose neither change. change
    // returns whether it changed anything (nothing is written otherwise).
    static bool Update(const std::filesystem::path& base, const std::function<bool(BrowserProfiles&)>& change,
        std::string& error);
    bool Save(const std::filesystem::path& base, std::string& error) const;

    // A profile's folder: base itself for the default profile.
    static std::filesystem::path Directory(const std::filesystem::path& base, const std::string& id);
    // The default profile's folder and the profile's identifier of a profile
    // folder: base/Profiles/<id> is profile id of base; any other folder is
    // a default profile.
    static std::pair<std::filesystem::path, std::string> Locate(const std::filesystem::path& directory);
    static bool ValidID(const std::string& id);
    // A name as typed: trimmed, at most 40 bytes (whole characters); empty
    // when nothing is left.
    static std::string CleanName(const std::string& name);

    const BrowserProfile* Find(const std::string& id) const;
    BrowserProfile* Find(const std::string& id);
    // Adds a profile; its identifier comes from its name ("Work" -> "work",
    // "work2" when taken). Returns it.
    const BrowserProfile& Add(const std::string& name, const std::string& color);
    bool Remove(const std::string& id);
    // The colour a new profile gets: the first that no profile has yet.
    std::string UnusedColor() const;
};
}
