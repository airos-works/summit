#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
    // History: the last visit of each earlier day the page was visited on,
    // newest first (kMaxEarlierVisits at most), so that History lists it
    // under each of those days.
    std::vector<int64_t> earlier {};
    // History: retained individual visits, in microseconds since the epoch.
    // Newest first; older profiles have only the second-resolution summaries.
    std::vector<int64_t> visitTimes {};
    // Known Firefox transition types (1..9), paired with retained visit times.
    // An absent pair means unknown, not a link visit. Equal times can have
    // several types after merging sources; no Firefox visit IDs are retained.
    std::vector<std::pair<int64_t, uint32_t>> visitTransitions {};
    // Firefox excludes downloads/reloads from visit_count. Distinguish its
    // real zero from the unknown zero in older Summit profiles.
    bool visitCountKnown = false;
};
constexpr size_t kMaxEarlierVisits = 60;
}
