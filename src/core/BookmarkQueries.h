#pragma once
#include "Bookmarks.h"
#include "HistoryVisits.h"

namespace summit {
// Saved Firefox Places searches. Results are transient: only the query URL is
// persisted. Unsupported clauses produce an explanation, never broader results.
bool IsBookmarkQueryURL(std::string_view url);
bool IsBookmarkFolderQueryURL(std::string_view url);
struct BookmarkQueryResult {
    std::vector<BookmarkNode> items;
    std::string error;
    // Individual-visit results only: one microsecond timestamp per item.
    // Presentation data, never saved into the bookmark tree.
    std::vector<int64_t> visitTimes = {};
    bool expandQueries = true;
};
std::string FormatBookmarkVisitTime(int64_t microseconds);
// Stable selectors for the four generated library links; zero is a saved query.
unsigned BookmarkQuerySectionForURL(std::string_view url);
struct BookmarkQueryTarget {
    BookmarkNode node;
    std::string error;
};
// Resolve a generated link through its saved parent, retaining source scope and
// checking that the parent still exposes it. Generated links are never saved.
BookmarkQueryTarget ResolveBookmarkQueryTarget(const Bookmarks&, const std::vector<PageRecord>&,
    int64_t id, unsigned section = 0);
BookmarkQueryResult QueryBookmarks(std::string_view url, const Bookmarks& bookmarks,
    const std::vector<PageRecord>& history, const BookmarkOrigin& origin = {}, int64_t now = HistoryNowMicroseconds());
}
