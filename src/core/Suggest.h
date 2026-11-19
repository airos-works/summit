#pragma once
#include "Profile.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace summit {
// Address field type-ahead, as Firefox does it: the site being typed is
// completed in the field itself, and a list under the field offers matching
// pages from history and bookmarks.

// What typing forward in the address field completes to. `text` is what the
// field shows: the typed characters as typed, followed by the completion (which
// the field selects, so the next key replaces it). `url` is where Enter goes.
// Both are empty when nothing is completed.
struct Autofill {
    std::string text;
    std::string url;
    bool Empty() const { return text.empty(); }
};

// Completes a typed site ("pul" -> "pulsetasmania.com/") or, once the text
// reaches into a path, the next path segment of a visited page
// ("github.com/jm" -> "github.com/jmgasper/"). Text with spaces, a search
// term, or nothing visited that starts that way is not completed.
Autofill AutofillAddress(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now);

struct Suggestion {
    enum class Kind { History, Bookmark };
    std::string url;
    std::string title;
    Kind kind = Kind::History;
    double score = 0;
};

struct AddressMatches {
    Autofill fill;
    std::vector<Suggestion> pages;
};

AddressMatches MatchAddress(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now, size_t limit, bool complete);

// Pages whose address or title contains every word typed (without case),
// best first: most visited and most recent, with sites that start with the
// text ahead of pages that only contain it.
std::vector<Suggestion> SuggestPages(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now, size_t limit);

// A history entry's weight: its visits, discounted by how long ago the last one was.
double Frecency(const PageRecord& page, int64_t now);

// The address as people read it: no scheme and no "www.", for the list.
std::string ShortAddress(std::string_view url);
}
