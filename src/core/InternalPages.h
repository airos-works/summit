#pragma once
#include "Profile.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace summit {
// Built-in pages. The browser writes each one to a file in the profile and
// shows it under its summit: address; links in them open in the same tab.
constexpr const char* kHistoryPage = "summit:history";
constexpr const char* kBookmarksPage = "summit:bookmarks";

// A visit of a page: its latest one, or the last of an earlier day.
struct HistoryVisit {
    const PageRecord* page;
    int64_t time;
};
struct HistoryDay {
    std::string label;
    std::vector<HistoryVisit> visits;
};
// Groups history's visits (most recent first) by local calendar day:
// "Today", "Yesterday", then the date, e.g. "Thursday, 24 September 2026". A
// page visited on several days is under each. Visits recorded before visits
// had times are grouped under "Earlier".
std::vector<HistoryDay> GroupHistoryByDay(const std::vector<PageRecord>& history, int64_t now);

// Returns an image URL (normally data:) for a page's favicon, or empty.
using IconSource = std::function<std::string(const std::string& pageURL)>;
std::string RenderHistoryPage(const std::vector<PageRecord>& history, int64_t now, const IconSource& icons);
// Both folders of bookmarks with their folders, to open, edit, delete and
// drag around. The page asks the browser for changes by navigating to
// summit-bookmarks: links carrying token (ParseBookmarkPageLink), which no
// other page knows.
std::string RenderBookmarksPage(const Bookmarks& bookmarks, const IconSource& icons, const std::string& token);
constexpr const char* kBookmarksPageScheme = "summit-bookmarks";
struct BookmarkPageLink {
    // save, delete, move, new-folder, import or export
    std::string action;
    std::string token;
    int64_t id = 0;
    int64_t folder = 0;
    size_t index = kBookmarkEnd;
    // save and new-folder: what was typed (percent-decoded).
    std::string title, url;
    bool hasTitle = false, hasURL = false;
    std::string description, tags;
    bool hasDescription = false, hasTags = false;
};
std::optional<BookmarkPageLink> ParseBookmarkPageLink(std::string_view url);
// The token as the page carries it: lower-case hex digits, at most 64.
std::string BookmarkPageToken(const std::string& token);
// What a private window's new tabs show: what private browsing keeps and
// forgets, and a search field for the chosen engine (SearchEngines()).
std::string RenderPrivateStartPage(const std::string& searchEngineName, const std::string& searchPrefix);
}
