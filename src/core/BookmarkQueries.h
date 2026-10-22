#pragma once
#include "Bookmarks.h"

namespace summit {
// Saved Firefox Places searches. Results are transient: only the query URL is
// persisted. Unsupported clauses produce an explanation, never broader results.
bool IsBookmarkQueryURL(std::string_view url);
struct BookmarkQueryResult {
    std::vector<BookmarkNode> items;
    std::string error;
};
BookmarkQueryResult QueryBookmarks(std::string_view url, const Bookmarks& bookmarks,
    const std::vector<PageRecord>& history, const BookmarkOrigin& origin = {});
}
