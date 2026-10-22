#include "BookmarkQueries.h"
#include "Address.h"
#include <algorithm>
#include <charconv>
#include <ctime>
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

struct Clause {
    int64_t minVisits = -1, maxVisits = -1;
    int64_t begin = 0, end = 0, beginRef = 0, endRef = 0;
    bool HasBegin() const { return begin || beginRef; }
    bool HasEnd() const { return end || endRef; }
    bool notTags = false, onlyBookmarked = false, domainIsHost = false;
    bool hasDomain = false;
    std::string terms, domain, uri;
    std::vector<std::string> tags;
    std::vector<std::pair<bool, std::string>> parents; // legacy numeric/symbolic folder, or GUID parent
    bool HasConditions(bool bookmarkQuery) const {
        return minVisits >= 0 || maxVisits >= 0 || HasBegin() || HasEnd() || hasDomain || !uri.empty()
            || !terms.empty() || !tags.empty() || !parents.empty() || (onlyBookmarked && !bookmarkQuery);
    }
};

struct Query {
    int64_t source = 0, sort = 0, type = 0, limit = 0;
    bool excludeQueries = false, excludeItems = false, expandQueries = true;
    std::string error;
    // Legacy Places OR separates conditions; presentation options are global.
    std::vector<Clause> clauses{1};
};

bool SimpleFolder(const Query& query)
{
    if (query.clauses.size() != 1) return false;
    const auto& clause = query.clauses.front();
    return clause.parents.size() == 1 && clause.terms.empty() && !clause.hasDomain && clause.uri.empty()
        && clause.tags.empty() && !query.limit && query.type == 0 && !clause.HasBegin() && !clause.HasEnd();
}

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
        auto& clause = q.clauses.back();
        if (name == "queryType") number(q.source, 0, 1);
        else if (name == "sort") number(q.sort, 0, 22);
        else if (name == "type") number(q.type, 0, 9);
        else if (name == "maxResults") number(q.limit, 0, UINT32_MAX);
        else if (name == "minVisits") number(clause.minVisits, -1, INT32_MAX);
        else if (name == "maxVisits") number(clause.maxVisits, -1, INT32_MAX);
        else if (name == "beginTime") number(clause.begin, INT64_MIN, INT64_MAX);
        else if (name == "endTime") number(clause.end, INT64_MIN, INT64_MAX);
        else if (name == "beginTimeRef") number(clause.beginRef, 0, 2);
        else if (name == "endTimeRef") number(clause.endRef, 0, 2);
        else if (name == "excludeQueries") flag(q.excludeQueries);
        else if (name == "excludeItems") flag(q.excludeItems);
        else if (name == "expandQueries") flag(q.expandQueries);
        else if (name == "!tags") flag(clause.notTags);
        else if (name == "onlyBookmarked") flag(clause.onlyBookmarked);
        else if (name == "domainIsHost") flag(clause.domainIsHost);
        else if (name == "domain") { text(clause.domain); clause.hasDomain = true; }
        else if (name == "uri") text(clause.uri);
        else if (name == "terms") text(clause.terms);
        else if (name == "parent" || name == "folder") {
            std::string parent; text(parent);
            if (parent.empty() || parent.size() > 128) q.error = "Invalid folder reference.";
            else if (std::find(clause.parents.begin(), clause.parents.end(), std::pair{name == "folder", parent}) == clause.parents.end())
                clause.parents.emplace_back(name == "folder", std::move(parent));
        }
        else if (name == "tag") {
            std::string tag; text(tag);
            if (std::find(clause.tags.begin(), clause.tags.end(), tag) == clause.tags.end()) clause.tags.push_back(std::move(tag));
        } else if (name == "OR") q.clauses.emplace_back();
        else if (name == "asyncEnabled") {
            bool ignored = false; flag(ignored); // Presentation/scheduling, not membership.
        } else if (name == "includeHidden" || name == "originalTitle") {
            bool enabled = false; flag(enabled);
            if (enabled) q.error = "Summit does not retain the data required by " + std::string(name) + ".";
        } else q.error = "This smart folder uses a query clause Summit does not yet support: " + std::string(name) + ".";
        if (!q.error.empty()) break;
    }
    if (q.error.empty() && q.type != 0 && q.type != 3 && q.type != 4 && q.type != 5 && q.type != 6)
        q.error = "This smart folder's result grouping is not yet supported.";
    if (q.error.empty() && q.sort != 0 && q.sort != 1 && q.sort != 2 && q.sort != 3 && q.sort != 4
        && q.sort != 5 && q.sort != 6 && q.sort != 7 && q.sort != 8 && q.sort != 11 && q.sort != 12
        && q.sort != 13 && q.sort != 14 && q.sort != 17 && q.sort != 18)
        q.error = "This smart folder's sort order requires data Summit does not yet retain.";
    if (q.type == 6 || std::any_of(q.clauses.begin(), q.clauses.end(), [](const auto& c) { return !c.parents.empty(); })) q.source = 1;
    if (q.error.empty() && q.type == 4) {
        const auto& c = q.clauses.front();
        // Places creates history containers even with queryType=1. Additional
        // filters have different SQL/container semantics from ordinary searches;
        // support only the verified complete time-range form for now.
        if (q.clauses.size() != 1 || c.minVisits >= 0 || c.maxVisits >= 0 || c.onlyBookmarked
            || !c.terms.empty() || !c.uri.empty() || !c.tags.empty() || !c.parents.empty()
            || c.HasBegin() != c.HasEnd() || (c.hasDomain && !c.HasBegin()))
            q.error = "Site grouping currently supports an unfiltered history query or a complete visit-time range, optionally restricted by domain.";
        q.source = 0;
    }
    if (q.error.empty() && (q.type == 3 || q.type == 5)) {
        if (q.clauses.size() != 1 || q.clauses.front().HasConditions(false))
            q.error = "Date grouping with additional conditions is not yet supported.";
        q.source = 0;
    }
    return q;
}

struct Entry {
    BookmarkNode node;
    const PageRecord* visit = nullptr;
};

struct DateRange {
    std::string title;
    int64_t begin, end, visibleBegin, visibleEnd;
};

bool DateRanges(int64_t now, std::vector<DateRange>& ranges)
{
    auto seconds = now / 1000000;
    if (now % 1000000 < 0) --seconds;
    const std::time_t clock = seconds;
    std::tm local {};
    if (int64_t(clock) != seconds || !localtime_r(&clock, &local)) return false;
    auto midnight = [&](int days, int months, bool monthStart, int64_t& value, std::tm* normalized = nullptr) {
        auto date = local;
        date.tm_hour = date.tm_min = date.tm_sec = 0;
        date.tm_isdst = -1;
        if (monthStart) date.tm_mday = 1;
        date.tm_mday += days;
        date.tm_mon += months;
        const auto time = std::mktime(&date);
        if (time == std::time_t(-1) || time > INT64_MAX / 1000000 || time < INT64_MIN / 1000000) return false;
        value = int64_t(time) * 1000000;
        if (normalized) *normalized = date;
        return true;
    };
    int64_t today, tomorrow, yesterday, week, month;
    if (!midnight(0, 0, false, today) || !midnight(1, 0, false, tomorrow)
        || !midnight(-1, 0, false, yesterday) || !midnight(-7, 0, false, week)
        || !midnight(0, 0, true, month)) return false;
    // Visibility is half-open. Generated Places child queries use inclusive
    // bounds, so an exact midnight can also appear in the preceding folder.
    ranges = {{"Today", today, tomorrow, today, tomorrow},
        {"Yesterday", yesterday, today, yesterday, today},
        {"Last 7 days", week, tomorrow, week, yesterday},
        {"This month", month, tomorrow, month, week}};
    constexpr const char* names[] = {"January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    auto end = month;
    for (int offset = 1; offset <= 5; ++offset) {
        int64_t begin;
        std::tm date {};
        if (!midnight(0, -offset, true, begin, &date)) return false;
        std::string title = names[date.tm_mon];
        if (date.tm_year < local.tm_year) title += " " + std::to_string(date.tm_year + 1900);
        ranges.push_back({std::move(title), begin, end, begin, end});
        end = begin;
    }
    // Firefox's SQLite datetime(0,'unixepoch') numeric conversion yields 1970,
    // then multiplies by a million. Preserve the verified generated bound.
    constexpr int64_t oldest = 1970000000;
    ranges.push_back({"Older than 6 months", oldest, end, oldest, end});
    return true;
}

std::vector<BookmarkNode> SiteGroups(const std::vector<const Entry*>& entries, bool showItems)
{
    std::map<std::string, BookmarkNode> groups;
    for (const auto* entry : entries) {
        auto host = Lower(Host(entry->node.url));
        if (host.empty() && Lower(entry->node.url.substr(0, 5)) != "file:") continue;
        auto& group = groups[host];
        group.folder = true;
        group.title = host.empty() ? "(local files)" : host;
        if (showItems) group.children.push_back(entry->node);
    }
    std::vector<BookmarkNode> result;
    for (auto& [host, group] : groups) result.push_back(std::move(group));
    return result;
}
}

BookmarkQueryResult QueryBookmarks(std::string_view url, const Bookmarks& bookmarks, const std::vector<PageRecord>& history,
    const BookmarkOrigin& origin, int64_t now)
{
    const auto q = Parse(url);
    BookmarkQueryResult result;
    result.error = q.error;
    if (!result.error.empty()) return result;
    auto resolveTime = [&](int64_t offset, int64_t reference, int64_t& time) {
        int64_t base = 0;
        if (reference == 2) base = now;
        else if (reference == 1) {
            std::time_t seconds = now / 1000000;
            std::tm local {};
            if (!localtime_r(&seconds, &local)) return false;
            local.tm_hour = local.tm_min = local.tm_sec = 0;
            local.tm_isdst = -1; // Midnight can have a different DST offset.
            const auto midnight = std::mktime(&local);
            if (midnight == std::time_t(-1) || midnight > INT64_MAX / 1000000 || midnight < INT64_MIN / 1000000)
                return false;
            base = int64_t(midnight) * 1000000;
        }
        if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && base < INT64_MIN - offset)) return false;
        time = base + offset;
        return true;
    };
    std::map<std::string, const PageRecord*> visits;
    for (const auto& page : history) visits.emplace(page.url, &page);
    std::map<std::string, const BookmarkNode*> saved;
    std::vector<const BookmarkNode*> ordered;
    std::map<int64_t, size_t> positions;
    std::function<void(const BookmarkNode&)> collect = [&](const BookmarkNode& folder) {
        size_t position = 0;
        for (const auto& child : folder.children) {
            positions.emplace(child.id, position++);
            if (child.folder) collect(child);
            else if (!child.separator) { ordered.push_back(&child); saved.emplace(child.url, &child); }
        }
    };
    collect(bookmarks.Bar()); collect(bookmarks.Other());
    struct PreparedClause {
        const Clause* condition;
        int64_t begin = INT64_MIN, end = INT64_MAX;
        std::vector<const BookmarkNode*> parents;
        std::set<int64_t> allowed;
        std::vector<std::string> terms;
    };
    const bool simpleFolder = SimpleFolder(q);
    std::vector<PreparedClause> filters;
    const bool constrained = std::any_of(q.clauses.begin(), q.clauses.end(), [&](const auto& clause) {
        return clause.HasConditions(q.source == 1);
    });
    for (const auto& clause : q.clauses) {
        // The legacy Places SQL builder omits empty branches when any branch
        // has conditions. Keep the original branch count for folder shortcuts.
        if (constrained && !clause.HasConditions(q.source == 1)) continue;
        PreparedClause filter{&clause, INT64_MIN, INT64_MAX, {}, {}, {}};
        if ((clause.HasBegin() && !resolveTime(clause.begin, clause.beginRef, filter.begin))
            || (clause.HasEnd() && !resolveTime(clause.end, clause.endRef, filter.end)))
            return {{}, "This smart folder's time range cannot be represented."};
        auto& parents = filter.parents;
        for (const auto& [legacy, reference] : clause.parents) {
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
        if (!parents.empty() && !simpleFolder) {
            // Firefox's constrained parent queries search descendants. Its simple
            // one-parent shortcut instead exposes the direct ordered folder tree.
            std::function<void(const BookmarkNode&)> descend = [&](const BookmarkNode& folder) {
                for (const auto& child : folder.children) {
                    if (child.folder) descend(child);
                    else if (!child.separator) filter.allowed.insert(child.id);
                }
            };
            for (const auto* parent : parents) descend(*parent);
        }
        auto text = Lower(clause.terms);
        for (size_t start = 0; start < text.size();) {
            start = text.find_first_not_of(" \t\r\n", start);
            if (start == std::string::npos) break;
            auto end = text.find_first_of(" \t\r\n", start);
            filter.terms.push_back(text.substr(start, end - start));
            start = end;
        }
        filters.push_back(std::move(filter));
    }
    std::vector<Entry> matches;
    auto accept = [&](const BookmarkNode& node, const PageRecord* visit) {
        if (!IsBookmarkableURL(node.url)) return;
        const bool query = IsBookmarkQueryURL(node.url);
        bool folderShortcut = false;
        if (query && q.excludeQueries) {
            const auto nested = Parse(node.url);
            folderShortcut = nested.error.empty() && SimpleFolder(nested);
        }
        if ((query && q.excludeQueries && !folderShortcut) || (!query && q.excludeItems && q.type == 0)) return;
        const int count = visit ? std::max(1, visit->visits) : 0;
        const auto search = Lower(node.title + " " + node.url + " " + BookmarkTagsText(node.metadata.tags));
        for (const auto& filter : filters) {
            const auto& clause = *filter.condition;
            if (!simpleFolder && !filter.parents.empty() && !filter.allowed.count(node.id)) continue;
            if (clause.onlyBookmarked && !saved.count(node.url)) continue;
            if ((clause.minVisits >= 0 && count < clause.minVisits) || (clause.maxVisits >= 0 && count > clause.maxVisits)) continue;
            if (clause.HasBegin() || clause.HasEnd()) {
                if (!visit) continue;
                const auto times = HistoryVisitTimes(*visit);
                if (std::none_of(times.begin(), times.end(), [&](int64_t t) { return t >= filter.begin && t <= filter.end; })) continue;
            }
            if (!clause.uri.empty() && node.url != clause.uri) continue;
            if (clause.hasDomain) {
                auto host = Lower(Host(node.url)), domain = Lower(clause.domain);
                if (host != domain && (clause.domainIsHost || domain.empty() || host.size() <= domain.size()
                    || host.compare(host.size() - domain.size(), domain.size(), domain) || host[host.size() - domain.size() - 1] != '.')) continue;
            }
            if (std::any_of(filter.terms.begin(), filter.terms.end(), [&](const auto& term) { return search.find(term) == std::string::npos; })) continue;
            if (std::any_of(clause.tags.begin(), clause.tags.end(), [&](const auto& tag) {
                    const bool has = std::find(node.metadata.tags.begin(), node.metadata.tags.end(), tag) != node.metadata.tags.end();
                    return has == clause.notTags;
                })) continue;
            // Each candidate is visited once. Overlapping OR branches cannot
            // duplicate it, while distinct bookmarks sharing a URL remain distinct.
            matches.push_back({node, visit});
            return;
        }
    };
    if (simpleFolder) {
        for (const auto& node : filters.front().parents.front()->children) {
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
            // General Places bookmark queries exclude saved queries; simple
            // folder shortcuts above retain them in their direct hierarchy.
            if (IsBookmarkQueryURL(node->url)) continue;
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
    const bool groupedHistory = q.type >= 3 && q.type <= 5;
    auto date = [](const Entry& e) { return e.visit ? (e.visit->visitTimes.empty()
        ? HistoryMicroseconds(e.visit->visited) : e.visit->visitTimes.front()) : int64_t(0); };
    auto count = [](const Entry& e) { return e.visit ? std::max(1, e.visit->visits) : 0; };
    auto sort = [&](int64_t mode) {
        if (!mode) return;
        std::stable_sort(matches.begin(), matches.end(), [&](const Entry& a, const Entry& b) {
            const bool descending = !(mode % 2);
            auto less = [&](const auto& x, const auto& y) { return descending ? y < x : x < y; };
            switch (mode) {
                case 1: case 2:
                    if (groupedHistory) {
                        if (Lower(a.node.title) == Lower(b.node.title)) return less(a.node.url, b.node.url);
                        return less(Lower(a.node.title), Lower(b.node.title));
                    }
                    return less(Lower(a.node.title.empty() ? a.node.url : a.node.title), Lower(b.node.title.empty() ? b.node.url : b.node.title));
                case 3: case 4:
                    if (groupedHistory && date(a) == date(b)) return less(Lower(a.node.title), Lower(b.node.title));
                    return less(date(a), date(b));
                case 5: case 6: return less(a.node.url, b.node.url);
                case 7: case 8:
                    if (groupedHistory && count(a) == count(b)) return less(date(a), date(b));
                    return less(count(a), count(b));
                case 11: case 12:
                    if (groupedHistory && a.node.added == b.node.added) return less(Lower(a.node.title), Lower(b.node.title));
                    return less(a.node.added, b.node.added);
                case 13: case 14:
                    if (a.node.modified != b.node.modified) return less(a.node.modified, b.node.modified);
                    if (Lower(a.node.title) != Lower(b.node.title)) return less(Lower(a.node.title), Lower(b.node.title));
                    return less(positions[a.node.id], positions[b.node.id]);
                case 17: case 18:
                    if (groupedHistory && a.node.metadata.tags == b.node.metadata.tags) {
                        if (Lower(a.node.title) == Lower(b.node.title)) return less(a.node.url, b.node.url);
                        return less(Lower(a.node.title), Lower(b.node.title));
                    }
                    return less(BookmarkTagsText(a.node.metadata.tags), BookmarkTagsText(b.node.metadata.tags));
                default: return false;
            }
        });
    };
    if (groupedHistory) {
        if (q.excludeQueries) return result;
        // Date/site descendants always use title order. Date-only descendants
        // inherit the requested order, defaulting to title when none is set.
        sort(q.type == 5 || (q.type == 3 && !q.sort) ? 1 : q.sort);
        if (q.type == 4) {
            std::vector<const Entry*> selected;
            for (const auto& entry : matches) selected.push_back(&entry);
            result.items = SiteGroups(selected, !q.excludeItems && q.expandQueries);
        } else {
            std::vector<DateRange> ranges;
            if (!DateRanges(now, ranges)) return {{}, "This smart folder's calendar ranges cannot be represented."};
            for (const auto& range : ranges) {
                bool visible = false;
                std::vector<const Entry*> selected;
                for (const auto& entry : matches) {
                    if (!entry.visit) continue;
                    bool member = false;
                    for (const auto time : HistoryVisitTimes(*entry.visit)) {
                        visible = visible || (time >= range.visibleBegin && time < range.visibleEnd);
                        member = member || (time >= range.begin && time <= range.end);
                    }
                    if (member) selected.push_back(&entry);
                }
                if (!visible) continue;
                BookmarkNode group;
                group.folder = true; group.title = range.title;
                if (q.type == 5) group.children = SiteGroups(selected, !q.excludeItems && q.expandQueries);
                else if (q.expandQueries && !q.excludeItems)
                    for (const auto* entry : selected) group.children.push_back(entry->node);
                result.items.push_back(std::move(group));
            }
        }
        // Places' SQL orders site URIs before LIMIT. Title sorts run after
        // LIMIT. Date containers retain their calendar order for other sorts.
        if (q.type == 4 && q.sort == 6) std::reverse(result.items.begin(), result.items.end());
        if (q.limit && result.items.size() > uint64_t(q.limit)) result.items.resize(q.limit);
        if (q.sort == 1 || q.sort == 2) {
            std::stable_sort(result.items.begin(), result.items.end(), [&](const auto& a, const auto& b) {
                return q.sort == 1 ? Lower(a.title) < Lower(b.title) : Lower(b.title) < Lower(a.title);
            });
        }
        return result;
    }
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
