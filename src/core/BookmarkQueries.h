#pragma once
#include "Bookmarks.h"
#include "HistoryVisits.h"

namespace summit {
// Saved Firefox Places searches. Results are transient: only the query URL is
// persisted. Unsupported clauses produce an explanation, never broader results.
bool IsBookmarkQueryURL(std::string_view url);
struct BookmarkQueryResult {
    std::vector<BookmarkNode> items;
    std::string error;
    // Individual-visit results only: one microsecond timestamp per item.
    // Presentation data, never saved into the bookmark tree.
    std::vector<int64_t> visitTimes = {};
};
std::string FormatBookmarkVisitTime(int64_t microseconds);
BookmarkQueryResult QueryBookmarks(std::string_view url, const Bookmarks& bookmarks,
    const std::vector<PageRecord>& history, const BookmarkOrigin& origin = {}, int64_t now = HistoryNowMicroseconds());
}
