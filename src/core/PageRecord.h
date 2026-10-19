#pragma once
#include <cstdint>
#include <string>

namespace summit {
// A page in history, a tab or a bookmark (Profile).
struct PageRecord {
    std::string url;
    std::string title;
    // History: when the page was last visited, in seconds since the epoch
    // (0 for entries saved before visits were timed).
    int64_t visited = 0;
    // Bookmarks: shown on the bookmarks bar under the toolbar.
    bool bar = false;
    // History: how many times the page was visited (0 for older entries,
    // which count as one). Address field type-ahead ranks pages by it.
    int visits = 0;
};
}
