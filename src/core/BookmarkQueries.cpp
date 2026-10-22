#include "BookmarkQueries.h"
#include "Address.h"
#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>

namespace summit {
bool IsBookmarkQueryURL(std::string_view url)
{
    if (url.size() < 6 || url.size() > 65536 || url.find('\0') != std::string_view::npos) return false;
    constexpr std::string_view scheme = "place:";
    for (size_t i = 0; i < scheme.size(); ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != scheme[i]) return false;
    }
    return true;
}

namespace {
std::string Lower(std::string text)
{
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}

std::string Host(std::string_view url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    url.remove_prefix(scheme + 3);
    url = url.substr(0, url.find_first_of("/?#"));
    if (auto at = url.rfind('@'); at != std::string_view::npos) url.remove_prefix(at + 1);
    if (!url.empty() && url.front() == '[') {
        const auto end = url.find(']');
        return end == std::string_view::npos ? std::string() : std::string(url.substr(1, end - 1));
    }
    return std::string(url.substr(0, url.find(':')));
}

bool Decode(std::string_view text, std::string& into)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    into.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return false;
            c = char(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        }
        if (!c) return false;
        into += c; // Places uses URI escaping: '+' is a literal plus.
    }
    return true;
}

struct Query {
    int64_t source = 0, sort = 0, type = 0, limit = 0, minVisits = -1, maxVisits = -1;
    bool excludeQueries = false, excludeItems = false, notTags = false, onlyBookmarked = false, domainIsHost = false;
    bool hasDomain = false;
    std::string terms, domain, uri, error;
    std::vector<std::string> tags;
    std::vector<std::pair<bool, std::string>> parents; // legacy numeric/symbolic folder, or GUID parent
};

Query Parse(std::string_view url)
{
    Query q;
    if (!IsBookmarkQueryURL(url)) { q.error = "This is not a Places query."; return q; }
    url.remove_prefix(6);
    size_t fields = 0;
    while (!url.empty()) {
        const auto end = url.find('&');
        const auto field = url.substr(0, end);
        url = end == std::string_view::npos ? std::string_view() : url.substr(end + 1);
        if (field.empty()) continue;
        if (++fields > 256) { q.error = "This smart folder has too many query clauses."; break; }
        const auto equal = field.find('=');
        const auto name = field.substr(0, equal);
        const auto value = equal == std::string_view::npos ? std::string_view() : field.substr(equal + 1);
        auto number = [&](int64_t& into, int64_t low, int64_t high) {
            int64_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size() || parsed < low || parsed > high)
                q.error = "Invalid value for " + std::string(name) + ".";
            else into = parsed;
        };
        auto flag = [&](bool& into) {
            if (value == "1" || value == "true") into = true;
            else if (value == "0" || value == "false") into = false;
            else q.error = "Invalid value for " + std::string(name) + ".";
        };
        auto text = [&](std::string& into) {
            if (!Decode(value, into)) q.error = "Invalid escaped text in " + std::string(name) + ".";
        };
        if (name == "queryType") number(q.source, 0, 1);
        else if (name == "sort") number(q.sort, 0, 22);
        else if (name == "type") number(q.type, 0, 9);
        else if (name == "maxResults") number(q.limit, 0, UINT32_MAX);
        else if (name == "minVisits") number(q.minVisits, -1, INT32_MAX);
        else if (name == "maxVisits") number(q.maxVisits, -1, INT32_MAX);
        else if (name == "excludeQueries") flag(q.excludeQueries);
        else if (name == "excludeItems") flag(q.excludeItems);
        else if (name == "!tags") flag(q.notTags);
        else if (name == "onlyBookmarked") flag(q.onlyBookmarked);
        else if (name == "domainIsHost") flag(q.domainIsHost);
        else if (name == "domain") { text(q.domain); q.hasDomain = true; }
        else if (name == "uri") text(q.uri);
        else if (name == "terms") text(q.terms);
        else if (name == "parent" || name == "folder") {
            std::string parent; text(parent);
            if (parent.empty() || parent.size() > 128) q.error = "Invalid folder reference.";
            else if (std::find(q.parents.begin(), q.parents.end(), std::pair{name == "folder", parent}) == q.parents.end())
                q.parents.emplace_back(name == "folder", std::move(parent));
        }
        else if (name == "tag") {
            std::string tag; text(tag);
            if (std::find(q.tags.begin(), q.tags.end(), tag) == q.tags.end()) q.tags.push_back(std::move(tag));
        } else if (name == "expandQueries" || name == "asyncEnabled") {
            bool ignored = false; flag(ignored); // Presentation/scheduling, not membership.
        } else if (name == "includeHidden" || name == "originalTitle") {
            bool enabled = false; flag(enabled);
            if (enabled) q.error = "Summit does not retain the data required by " + std::string(name) + ".";
        } else q.error = "This smart folder uses a query clause Summit does not yet support: " + std::string(name) + ".";
        if (!q.error.empty()) break;
    }
    if (q.error.empty() && q.type != 0 && q.type != 6)
        q.error = "This smart folder's result grouping is not yet supported.";
    if (q.error.empty() && q.sort != 0 && q.sort != 1 && q.sort != 2 && q.sort != 3 && q.sort != 4
        && q.sort != 5 && q.sort != 6 && q.sort != 7 && q.sort != 8 && q.sort != 11 && q.sort != 12
        && q.sort != 17 && q.sort != 18)
        q.error = "This smart folder's sort order requires data Summit does not yet retain.";
    if (q.type == 6 || !q.parents.empty()) q.source = 1;
    return q;
}

struct Entry {
    BookmarkNode node;
    const PageRecord* visit = nullptr;
};
}

BookmarkQueryResult QueryBookmarks(std::string_view url, const Bookmarks& bookmarks, const std::vector<PageRecord>& history,
    const BookmarkOrigin& origin)
{
    const auto q = Parse(url);
    BookmarkQueryResult result;
    result.error = q.error;
    if (!result.error.empty()) return result;
    std::map<std::string, const PageRecord*> visits;
    for (const auto& page : history) visits.emplace(page.url, &page);
    std::map<std::string, const BookmarkNode*> saved;
    std::vector<const BookmarkNode*> ordered;
    std::function<void(const BookmarkNode&)> collect = [&](const BookmarkNode& folder) {
        for (const auto& child : folder.children) {
            if (child.folder) collect(child);
            else if (!child.separator) { ordered.push_back(&child); saved.emplace(child.url, &child); }
        }
    };
    collect(bookmarks.Bar()); collect(bookmarks.Other());
    std::vector<const BookmarkNode*> parents;
    for (const auto& [legacy, reference] : q.parents) {
        std::string guid = reference;
        int64_t numeric = 0;
        if (legacy) {
            if (reference == "BOOKMARKS_MENU") guid = "menu________";
            else if (reference == "TOOLBAR") guid = "toolbar_____";
            else if (reference == "UNFILED_BOOKMARKS") guid = "unfiled_____";
            else if (reference == "PLACES_ROOT") guid = "root________";
            else if (reference == "MOBILE_BOOKMARKS") guid = "mobile______";
            else {
                auto parsed = std::from_chars(reference.data(), reference.data() + reference.size(), numeric);
                if (parsed.ec != std::errc() || parsed.ptr != reference.data() + reference.size() || numeric <= 0)
                    return {{}, "Invalid folder reference: " + reference + "."};
                guid.clear();
            }
        }
        const BookmarkNode* found = nullptr;
        bool ambiguous = false;
        std::function<void(const BookmarkNode&)> locate = [&](const BookmarkNode& node) {
            if (!node.folder) return;
            bool matches = false;
            if (!origin.scope.empty()) matches = node.origin.scope == origin.scope
                && (numeric ? node.origin.id == numeric : node.origin.guid == guid);
            else matches = (guid == "toolbar_____" && node.id == kBookmarksBarFolder)
                || (guid == "unfiled_____" && node.id == kOtherBookmarksFolder);
            if (matches) { if (found) ambiguous = true; else found = &node; }
            for (const auto& child : node.children) locate(child);
        };
        locate(bookmarks.Bar()); locate(bookmarks.Other());
        if (ambiguous) return {{}, "This smart folder's target is ambiguous: " + reference + "."};
        if (!found) return {{}, "This smart folder's target is missing or was not imported: " + reference + "."};
        parents.push_back(found);
    }
    const bool simpleFolder = parents.size() == 1 && q.terms.empty() && !q.hasDomain && q.uri.empty()
        && q.tags.empty() && !q.limit && q.type == 0;
    std::set<int64_t> allowed;
    if (!parents.empty() && !simpleFolder) {
        // Firefox's constrained parent queries search descendants. Its simple
        // one-parent shortcut instead exposes the direct ordered folder tree.
        std::function<void(const BookmarkNode&)> descend = [&](const BookmarkNode& folder) {
            for (const auto& child : folder.children) {
                if (child.folder) descend(child);
                else if (!child.separator) allowed.insert(child.id);
            }
        };
        for (const auto* parent : parents) descend(*parent);
    }
    std::vector<std::string> terms;
    auto text = Lower(q.terms);
    for (size_t start = 0; start < text.size();) {
        start = text.find_first_not_of(" \t\r\n", start);
        if (start == std::string::npos) break;
        auto end = text.find_first_of(" \t\r\n", start);
        terms.push_back(text.substr(start, end - start));
        start = end;
    }
    std::vector<Entry> matches;
    auto accept = [&](const BookmarkNode& node, const PageRecord* visit) {
        if (!IsBookmarkableURL(node.url)) return;
        const bool query = IsBookmarkQueryURL(node.url);
        bool folderShortcut = false;
        if (query && q.excludeQueries) {
            const auto nested = Parse(node.url);
            folderShortcut = nested.error.empty() && nested.parents.size() == 1 && nested.terms.empty()
                && !nested.hasDomain && nested.uri.empty() && nested.tags.empty() && !nested.limit && nested.type == 0;
        }
        if ((query && q.excludeQueries && !folderShortcut) || (!query && q.excludeItems && q.type == 0)
            || (q.onlyBookmarked && !saved.count(node.url))) return;
        const int count = visit ? std::max(1, visit->visits) : 0;
        if ((q.minVisits >= 0 && count < q.minVisits) || (q.maxVisits >= 0 && count > q.maxVisits)) return;
        if (!q.uri.empty() && node.url != q.uri) return;
        if (q.hasDomain) {
            auto host = Lower(Host(node.url)), domain = Lower(q.domain);
            if (host != domain && (q.domainIsHost || domain.empty() || host.size() <= domain.size()
                || host.compare(host.size() - domain.size(), domain.size(), domain) || host[host.size() - domain.size() - 1] != '.')) return;
        }
        const auto search = Lower(node.title + " " + node.url + " " + BookmarkTagsText(node.metadata.tags));
        for (const auto& term : terms) if (search.find(term) == std::string::npos) return;
        for (const auto& tag : q.tags) {
            const bool has = std::find(node.metadata.tags.begin(), node.metadata.tags.end(), tag) != node.metadata.tags.end();
            if (has == q.notTags) return;
        }
        matches.push_back({node, visit});
    };
    if (simpleFolder) {
        for (const auto& node : parents.front()->children) {
            const bool query = IsBookmarkQueryURL(node.url);
            if (node.folder || node.separator) {
                if (!node.separator || (!q.excludeItems && !q.sort)) matches.push_back({node, nullptr});
            } else if (!q.excludeItems || query) {
                auto found = visits.find(node.url);
                accept(node, found == visits.end() ? nullptr : found->second);
            }
        }
    } else if (q.source == 1) {
        for (const auto* node : ordered) {
            if (!parents.empty() && !allowed.count(node->id)) continue;
            auto found = visits.find(node->url);
            accept(*node, found == visits.end() ? nullptr : found->second);
        }
    } else {
        for (const auto& page : history) {
            BookmarkNode node{0, page.title, page.url, false, 0, {}};
            if (auto found = saved.find(page.url); found != saved.end()) node.metadata = found->second->metadata;
            accept(node, &page);
        }
    }
    auto date = [](const Entry& e) { return e.visit ? e.visit->visited : int64_t(0); };
    auto count = [](const Entry& e) { return e.visit ? std::max(1, e.visit->visits) : 0; };
    auto sort = [&](int64_t mode) {
        if (!mode) return;
        std::stable_sort(matches.begin(), matches.end(), [&](const Entry& a, const Entry& b) {
            const bool descending = !(mode % 2);
            auto less = [&](const auto& x, const auto& y) { return descending ? y < x : x < y; };
            switch (mode) {
                case 1: case 2: return less(Lower(a.node.title.empty() ? a.node.url : a.node.title), Lower(b.node.title.empty() ? b.node.url : b.node.title));
                case 3: case 4: return less(date(a), date(b));
                case 5: case 6: return less(a.node.url, b.node.url);
                case 7: case 8: return less(count(a), count(b));
                case 11: case 12: return less(a.node.added, b.node.added);
                case 17: case 18: return less(BookmarkTagsText(a.node.metadata.tags), BookmarkTagsText(b.node.metadata.tags));
                default: return false;
            }
        });
    };
    if (q.type == 6) {
        // Tags are containers, with URL results beneath them. Do not persist
        // the expansion or recursively evaluate saved queries here.
        std::map<std::string, BookmarkNode> groups;
        std::map<std::string, std::set<std::string>> groupedURLs;
        size_t members = 0;
        for (const auto& entry : matches) {
            if (IsBookmarkQueryURL(entry.node.url)) continue;
            for (const auto& tag : entry.node.metadata.tags) {
                auto& group = groups[tag]; group.folder = true; group.title = tag;
                if (groupedURLs[tag].insert(entry.node.url).second) {
                    if (++members > kMaxBookmarks)
                        return {{}, "This smart folder has too many tagged results to display."};
                    // A result needs its identity and address, not another
                    // copy of every tag on that address for each tag folder.
                    group.children.push_back({entry.node.id, entry.node.title, entry.node.url, false, entry.node.added, {}});
                }
            }
        }
        if (q.sort != 0 && q.sort != 1 && q.sort != 2) {
            result.error = "Sorting tag folders by this property is not yet supported.";
            return result;
        }
        for (auto& [tag, group] : groups) result.items.push_back(std::move(group));
        if (q.sort == 2) std::reverse(result.items.begin(), result.items.end());
        if (q.limit && result.items.size() > uint64_t(q.limit)) result.items.resize(q.limit);
        return result;
    }
    // Firefox chooses the newest maxResults before sorting by title.
    sort(q.limit && (q.sort == 1 || q.sort == 2) ? 4 : q.sort);
    if (q.limit && matches.size() > uint64_t(q.limit)) matches.resize(q.limit);
    if (q.limit && (q.sort == 1 || q.sort == 2)) sort(q.sort);
    for (auto& entry : matches) result.items.push_back(std::move(entry.node));
    return result;
}
}
