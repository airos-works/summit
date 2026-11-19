#include "Suggest.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <unordered_map>

namespace summit {
namespace {
std::string Lower(std::string_view text)
{
    std::string result(text);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

// Views into the lowercased URL. Keep the path in its original case for completion.
struct Parts {
    std::string_view scheme, authority;
    std::string path;
    size_t pathStart;

    bool Split(std::string_view address, std::string_view original, bool needPath)
    {
        size_t start;
        if (address.starts_with("https://")) start = 8;
        else if (address.starts_with("http://")) start = 7;
        else return false;
        scheme = address.substr(0, start);
        pathStart = address.find_first_of("/?#", start);
        authority = address.substr(start, pathStart - start);
        if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
        if (needPath) {
            path.clear();
            if (pathStart == std::string_view::npos || address[pathStart] != '/') path += '/';
            if (pathStart != std::string_view::npos) path.append(original.substr(pathStart));
        }
        return true;
    }
};

std::string_view WithoutWWW(std::string_view authority)
{
    return authority.starts_with("www.") ? authority.substr(4) : authority;
}

// A bookmark without visits still counts, as a page visited once recently.
constexpr double kBookmarkWeight = 100;

std::vector<std::string> Words(std::string_view text)
{
    std::vector<std::string> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        size_t end = i;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end > i) words.push_back(Lower(text.substr(i, end - i)));
        i = end;
    }
    return words;
}
}

double Frecency(const PageRecord& page, int64_t now)
{
    const double visits = std::max(1, page.visits);
    const double days = page.visited ? std::max<int64_t>(0, now - page.visited) / 86400.0 : 365;
    const double weight = days < 4 ? 100 : days < 14 ? 70 : days < 31 ? 50 : days < 90 ? 30 : 10;
    return visits * weight;
}

namespace {
// Keep the searchable text and URL parts in scratch storage across pages.
struct SearchText : Parts {
    std::array<char, 256> lower;
    std::string url, haystack;
    std::string_view site;

    SearchText()
    {
        for (size_t i = 0; i < lower.size(); ++i)
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(i)));
    }

    bool Set(const PageRecord& page, bool needPath)
    {
        url.resize(page.url.size());
        std::transform(page.url.begin(), page.url.end(), url.begin(), [&](unsigned char c) { return lower[c]; });
        if (!Split(url, page.url, needPath)) return false;
        site = WithoutWWW(authority);
        return true;
    }

    void SetHaystack(const std::string& title)
    {
        haystack = site;
        if (pathStart == std::string_view::npos || url[pathStart] != '/') haystack += '/';
        if (pathStart != std::string_view::npos) haystack.append(url, pathStart);
        haystack += '\n';
        for (unsigned char c : title) haystack += lower[c];
    }
};

class InlineCompletion {
public:
    InlineCompletion(std::string_view typed, bool enabled) : fTyped(typed)
    {
        if (!enabled || typed.empty() || typed.size() > 2048
            || std::any_of(typed.begin(), typed.end(), [](unsigned char c) { return std::isspace(c); })) return;
        fWanted = Lower(typed);
        if (fWanted.starts_with("https://")) fScheme = "https://";
        else if (fWanted.starts_with("http://")) fScheme = "http://";
        fWanted.erase(0, fScheme.size());
        if (fWanted.starts_with('/')) fWanted.clear();
        fSlash = fWanted.find('/');
        fWWW = fWanted.starts_with("www.");
    }

    bool NeedsPath() const { return fSlash != std::string::npos; }
    bool Active() const { return !fWanted.empty(); }

    void Add(const SearchText& text, double weight)
    {
        if (!fScheme.empty() && fScheme != text.scheme) return;
        const auto shown = fWWW ? text.authority : text.site;
        if (fSlash == std::string::npos) {
            if (!shown.starts_with(fWanted)) return;
            fCompletion = shown;
            fCompletion += '/';
        } else {
            const std::string_view wanted(fWanted);
            if (shown != wanted.substr(0, fSlash)) return;
            const auto prefix = wanted.substr(fSlash);
            if (text.path.size() <= prefix.size()) return;
            for (size_t i = 0; i < prefix.size(); ++i)
                if (text.lower[static_cast<unsigned char>(text.path[i])] != prefix[i]) return;
            auto end = text.path.find('/', prefix.size());
            if (end != std::string::npos) ++end;
            else end = std::min(text.path.find_first_of("?#", prefix.size()), text.path.size());
            if (end <= prefix.size()) return;
            fCompletion = shown;
            fCompletion.append(text.path, 0, end);
        }
        auto& group = fGroups[fCompletion];
        group.weight += weight;
        if (weight > group.best) {
            group.best = weight;
            group.url = text.scheme;
            group.url += text.authority;
            group.url.append(fCompletion, shown.size());
        }
    }

    Autofill Result() const
    {
        decltype(fGroups)::const_iterator winner = fGroups.end();
        for (auto it = fGroups.begin(); it != fGroups.end(); ++it) {
            if (winner == fGroups.end() || it->second.weight > winner->second.weight
                || (it->second.weight == winner->second.weight && it->first.size() < winner->first.size()))
                winner = it;
        }
        if (winner == fGroups.end() || winner->first.size() <= fWanted.size()) return {};
        return {std::string(fTyped) + winner->first.substr(fWanted.size()), winner->second.url};
    }

private:
    struct Group {
        double weight = 0, best = -1;
        std::string url;
    };
    std::string_view fTyped, fScheme;
    std::string fWanted, fCompletion;
    size_t fSlash = std::string::npos;
    bool fWWW = false;
    std::map<std::string, Group> fGroups;
};

struct RankedPage {
    const PageRecord* page;
    const std::string* title;
    Suggestion::Kind kind;
    double score;
    size_t order;
};

double MatchWeight(SearchText& text, const std::string& title, const std::vector<std::string>& words)
{
    const bool sitePrefix = text.site.starts_with(words.front());
    if (words.size() == 1 && sitePrefix) return 4;
    text.SetHaystack(title);
    bool boundary = false;
    for (const auto& word : words) {
        const size_t found = text.haystack.find(word);
        if (found == std::string::npos) return 0;
        if (!found || !std::isalnum(static_cast<unsigned char>(text.haystack[found - 1]))) boundary = true;
    }
    if (sitePrefix) return 4;
    return boundary ? 1.5 : 1;
}
}

AddressMatches MatchAddress(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now, size_t limit, bool complete)
{
    InlineCompletion inlineCompletion(typed, complete);
    const auto words = limit ? Words(typed) : std::vector<std::string>{};
    if (words.empty() && !inlineCompletion.Active()) return {};

    SearchText text;
    std::vector<RankedPage> matches;
    std::unordered_map<std::string_view, size_t> seen;
    const auto scan = [&](const std::vector<PageRecord>& pages, Suggestion::Kind kind) {
        for (const auto& page : pages) {
            if (!text.Set(page, inlineCompletion.NeedsPath())) continue;
            const double weight = kind == Suggestion::Kind::Bookmark ? kBookmarkWeight : Frecency(page, now);
            if (inlineCompletion.Active()) inlineCompletion.Add(text, weight);
            if (words.empty()) continue;
            const double score = weight * MatchWeight(text, page.title, words);
            if (!score) continue;
            if (seen.empty()) seen.reserve(history.size() + bookmarks.size());
            const auto [it, inserted] = seen.try_emplace(page.url, matches.size());
            if (inserted) matches.push_back({&page, &page.title, kind, score, matches.size()});
            else {
                auto& row = matches[it->second];
                row.score += score;
                if (kind == Suggestion::Kind::Bookmark) row.kind = kind;
                if (row.title->empty()) row.title = &page.title;
            }
        }
    };
    scan(history, Suggestion::Kind::History);
    scan(bookmarks, Suggestion::Kind::Bookmark);

    const auto count = std::min(limit, matches.size());
    std::partial_sort(matches.begin(), matches.begin() + count, matches.end(), [](const auto& a, const auto& b) {
        return a.score == b.score ? a.order < b.order : a.score > b.score;
    });
    AddressMatches result {inlineCompletion.Result(), {}};
    result.pages.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& row = matches[i];
        result.pages.push_back({row.page->url, *row.title, row.kind, row.score});
    }
    return result;
}

Autofill AutofillAddress(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now)
{
    return MatchAddress(history, bookmarks, typed, now, 0, true).fill;
}

std::vector<Suggestion> SuggestPages(const std::vector<PageRecord>& history, const std::vector<PageRecord>& bookmarks,
    std::string_view typed, int64_t now, size_t limit)
{
    return MatchAddress(history, bookmarks, typed, now, limit, false).pages;
}

std::string ShortAddress(std::string_view url)
{
    Parts parts;
    const auto lower = Lower(url);
    if (!parts.Split(lower, url, true)) return std::string(url);
    std::string result(WithoutWWW(parts.authority));
    if (parts.path != "/") result += parts.path;
    return result;
}
}
