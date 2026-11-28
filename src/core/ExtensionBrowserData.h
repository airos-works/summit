#pragma once
// The browser's data for extensions: chrome.history and chrome.bookmarks, and
// the bookmark events (ExtensionBrowserData.cpp).
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace summit {

// The profile as the browser keeps it: air/OS's SharedProfile, Android's in
// SummitCoreJni.
class ProfileStore {
public:
    enum class Kind { History, Bookmarks };
    virtual ~ProfileStore() = default;
    // Runs function under the profile's lock.
    virtual void Read(const std::function<void(const Profile&)>&) const = 0;
    // Runs function under the profile's lock; when it returns true, the
    // change is saved and announced as the browser does for its own.
    virtual void Change(Kind, const std::function<bool(Profile&)>&) = 0;
    virtual void ClearHistory() = 0;
};

// What function returns, read under the profile's lock.
template<typename Function> auto ReadProfile(const ProfileStore& store, Function&& function)
{
    std::optional<std::invoke_result_t<Function, const Profile&>> result;
    store.Read([&](const Profile& profile) { result.emplace(function(profile)); });
    return std::move(*result);
}

// The answer to an extension's browser command.
struct BrowserDataAnswer {
    enum class Status { Ok, BadValue, NotSupported };
    Status status = Status::Ok;
    std::string data = "null"; // JSON
    std::string error;
    // history.onVisited and onVisitRemoved: ("visited" or "removed", JSON).
    std::vector<std::pair<std::string, std::string>> historyEvents;
    // The bookmarks changed: the host reports the events
    // (BookmarkChangeReporter) before it answers, as Chrome does.
    bool bookmarksChanged = false;
    static BrowserDataAnswer Failure(Status status)
    {
        BrowserDataAnswer answer;
        answer.status = status;
        return answer;
    }
};

// chrome.history's HistoryItem for a page; times in milliseconds, as in Chrome.
nlohmann::json HistoryItemJSON(const PageRecord&);
// chrome.history (method: search, getVisits, addUrl, deleteUrl, deleteRange, deleteAll).
BrowserDataAnswer ExtensionHistoryCommand(ProfileStore&, const std::string& method, const std::string& arguments);
// chrome.bookmarks (getTree, get, getChildren, getSubTree, getRecent, search,
// create, move, update, remove, removeTree).
BrowserDataAnswer ExtensionBookmarksCommand(ProfileStore&, const std::string& method, const std::string& arguments);

// What extensions see of each bookmark node, for comparing two states.
struct BookmarkEntry {
    int64_t parent = 0;
    size_t index = 0;
    std::string title, url;
    // Without children; a removed folder's are put together from the others.
    nlohmann::json node;
};
using BookmarkEntries = std::map<int64_t, BookmarkEntry>;

// bookmarks.onCreated, onRemoved, onChanged and onMoved: the changes since the
// last call (the first call only records the bookmarks).
class BookmarkChangeReporter {
public:
    struct Event {
        const char* kind; // "created", "removed", "changed", "moved"
        int64_t id;
        nlohmann::json info;
    };
    std::vector<Event> Changes(const Bookmarks&);
    void Reset() { fReported.reset(); }

private:
    std::optional<BookmarkEntries> fReported;
};

} // namespace summit
