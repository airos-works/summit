// The browser's data for extensions: chrome.history and chrome.bookmarks
// (#106), and the bookmark events. Shared by air/OS (BrowserWindowFeatures.cpp,
// over SharedProfile) and Android (SummitExtensionsJni.cpp, over its profile),
// so that both answer extensions alike.
#include "core/ExtensionBrowserData.h"

#include "core/Address.h"
#include "core/HistoryVisits.h"
#include "core/PageLanguage.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>
#include <sstream>

namespace summit {

namespace {
nlohmann::json HistoryItemJSONInternal(const PageRecord& page)
{
    const auto times = HistoryVisitTimes(page);
    const double last = times.empty() ? page.visited * 1000.0 : times.front() / 1000.0;
    return {
        {"id", std::to_string(std::hash<std::string>()(page.url))},
        {"url", page.url},
        {"title", page.title},
        {"lastVisitTime", last},
        {"visitCount", HistoryVisitCount(page)},
        {"typedCount", 0},
    };
}

nlohmann::json BookmarkJSON(const BookmarkNode& node, int64_t parent, size_t index, int depth)
{
    nlohmann::json item = {
        {"id", std::to_string(node.id)},
        {"parentId", std::to_string(parent)},
        {"index", index},
        {"title", node.separator ? std::string() : node.title},
        {"dateAdded", node.added * 1000.0},
        {"type", node.separator ? "separator" : node.folder ? "folder" : "bookmark"},
    };
    if (node.folder) {
        if (node.id == kBookmarksBarFolder) item["folderType"] = "bookmarks-bar";
        else if (node.id == kOtherBookmarksFolder) item["folderType"] = "other";
        if (node.modified) item["dateGroupModified"] = node.modified / 1000.0;
        if (depth) {
            auto children = nlohmann::json::array();
            for (size_t i = 0; i < node.children.size(); ++i)
                children.push_back(BookmarkJSON(node.children[i], node.id, i, depth - 1));
            item["children"] = std::move(children);
        }
    } else if (!node.separator)
        item["url"] = node.url;
    return item;
}

nlohmann::json BookmarkRootJSON(const Bookmarks& bookmarks, int depth)
{
    nlohmann::json root = {{"id", "0"}, {"title", ""}, {"dateAdded", 0}, {"type", "folder"}};
    if (depth)
        root["children"] = {BookmarkJSON(bookmarks.Bar(), 0, 0, depth - 1), BookmarkJSON(bookmarks.Other(), 0, 1, depth - 1)};
    return root;
}

// A node and where it is; the root and the two fixed folders have parent 0.
std::optional<nlohmann::json> FindBookmarkJSON(const Bookmarks& bookmarks, int64_t id, int depth)
{
    if (!id) return BookmarkRootJSON(bookmarks, depth);
    const auto* node = bookmarks.Find(id);
    if (!node) return std::nullopt;
    size_t index = 0;
    const auto* parent = bookmarks.Parent(id, &index);
    if (!parent) index = id == kBookmarksBarFolder ? 0 : 1;
    return BookmarkJSON(*node, parent ? parent->id : 0, index, depth);
}

std::optional<int64_t> BookmarkID(const std::string& text)
{
    if (text.empty() || text.size() > 18 || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return std::stoll(text);
}

std::string LowerText(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

void EachBookmark(const BookmarkNode& folder, const std::function<void(const BookmarkNode&, int64_t, size_t)>& visit)
{
    for (size_t i = 0; i < folder.children.size(); ++i) {
        visit(folder.children[i], folder.id, i);
        if (folder.children[i].folder) EachBookmark(folder.children[i], visit);
    }
}


BookmarkEntries BookmarkSnapshot(const Bookmarks& bookmarks)
{
    BookmarkEntries entries;
    for (const auto* folder : {&bookmarks.Bar(), &bookmarks.Other()}) {
        EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
            entries[node.id] = {parent, index, node.title, node.url, BookmarkJSON(node, parent, index, 0)};
        });
    }
    return entries;
}

// A node of entries with its children, as onRemoved reports a folder.
nlohmann::json BookmarkTreeJSON(const BookmarkEntries& entries, const std::multimap<int64_t, int64_t>& children, int64_t id)
{
    auto node = entries.at(id).node;
    if (node.value("type", std::string()) != "folder") return node;
    std::vector<int64_t> inside;
    for (auto [it, end] = children.equal_range(id); it != end; ++it) inside.push_back(it->second);
    std::sort(inside.begin(), inside.end(), [&](int64_t a, int64_t b) { return entries.at(a).index < entries.at(b).index; });
    auto list = nlohmann::json::array();
    for (auto child : inside) list.push_back(BookmarkTreeJSON(entries, children, child));
    node["children"] = std::move(list);
    return node;
}

} // namespace

nlohmann::json HistoryItemJSON(const PageRecord& page)
{
    return HistoryItemJSONInternal(page);
}

BrowserDataAnswer ExtensionHistoryCommand(ProfileStore& store, const std::string& method, const std::string& argumentText)
{
    BrowserDataAnswer answer;
    const auto arguments = nlohmann::json::parse(argumentText.empty() ? std::string("{}") : argumentText, nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { return BrowserDataAnswer::Failure(BrowserDataAnswer::Status::BadValue); }
    auto respond = [&](const nlohmann::json& data) { answer.data = data.dump(); };
    auto removed = [&](bool all, const std::vector<std::string>& urls) {
        if (all || !urls.empty())
            answer.historyEvents.emplace_back("removed", nlohmann::json({{"allHistory", all}, {"urls", urls}}).dump());
    };
    const double now = HistoryNowMicroseconds() / 1000.0;
    if (method == "search") {
        std::string text = arguments.value("text", std::string());
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        // Chrome's defaults: the last 24 hours, at most 100 items.
        const double start = arguments.value("startTime", now - 24 * 3600 * 1000.0);
        const double end = arguments.value("endTime", now);
        const size_t limit = static_cast<size_t>(std::clamp(arguments.value("maxResults", 100.0), 0.0, 100000.0));
        auto items = nlohmann::json::array();
        store.Read([&](const Profile& profile) {
            for (const auto& page : profile.history) {
                if (items.size() >= limit) break;
                auto item = HistoryItemJSON(page);
                const double last = item["lastVisitTime"];
                if (last < start || last > end) continue;
                if (!text.empty()) {
                    std::string haystack = page.url + "\n" + page.title;
                    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (haystack.find(text) == std::string::npos) continue;
                }
                items.push_back(std::move(item));
            }
        });
        respond(items);
    } else if (method == "getVisits") {
        const std::string url = arguments.value("url", std::string());
        auto visits = nlohmann::json::array();
        store.Read([&](const Profile& profile) {
            for (const auto& page : profile.history) {
                if (page.url != url) continue;
                const auto id = std::to_string(std::hash<std::string>()(page.url));
                // Chrome's TransitionType for the visit's Firefox type.
                auto transition = [&](int64_t time) {
                    for (const auto& [visitTime, type] : page.visitTransitions) {
                        if (visitTime != time) continue;
                        switch (type) {
                            case 2: return "typed";
                            case 3: return "auto_bookmark";
                            case 4: return "auto_subframe";
                            case 8: return "manual_subframe";
                            case 9: return "reload";
                            default: return "link";
                        }
                    }
                    return "link";
                };
                for (auto time : HistoryVisitTimes(page))
                    visits.push_back({{"id", id}, {"visitId", std::to_string(time)}, {"visitTime", time / 1000.0}, {"referringVisitId", "0"}, {"transition", transition(time)}, {"isLocal", true}});
                if (visits.empty() && page.visited)
                    visits.push_back({{"id", id}, {"visitId", std::to_string(page.visited)}, {"visitTime", page.visited * 1000.0}, {"referringVisitId", "0"}, {"transition", "link"}, {"isLocal", true}});
                break;
            }
        });
        respond(visits);
    } else if (method == "addUrl") {
        PageRecord page { arguments.value("url", std::string()), arguments.value("title", std::string()) };
        if (page.url.empty()) { return BrowserDataAnswer::Failure(BrowserDataAnswer::Status::BadValue); }
        std::optional<PageRecord> recorded;
        const double time = arguments.value("visitTime", now);
        store.Change(ProfileStore::Kind::History, [&](Profile& profile) -> bool {
            if (!profile.Visit(page, static_cast<int64_t>(time / 1000))) return false;
            for (const auto& entry : profile.history) {
                if (entry.url == page.url) { recorded = entry; break; }
            }
            return true;
        });
        if (recorded) answer.historyEvents.emplace_back("visited", HistoryItemJSON(*recorded).dump());
        respond(nullptr);
    } else if (method == "deleteUrl") {
        const std::string url = arguments.value("url", std::string());
        bool erased = false;
        store.Change(ProfileStore::Kind::History, [&](Profile& profile) -> bool {
            erased = std::erase_if(profile.history, [&](const PageRecord& page) { return page.url == url; }) > 0;
            return erased ? true : false;
        });
        if (erased) removed(false, { url });
        respond(nullptr);
    } else if (method == "deleteRange") {
        // Pages last visited in the range; their earlier visits go with them.
        const double start = arguments.value("startTime", 0.0), end = arguments.value("endTime", 0.0);
        std::vector<std::string> urls;
        store.Change(ProfileStore::Kind::History, [&](Profile& profile) -> bool {
            std::erase_if(profile.history, [&](const PageRecord& page) {
                const double last = HistoryItemJSON(page)["lastVisitTime"];
                if (last < start || last > end) return false;
                urls.push_back(page.url);
                return true;
            });
            return urls.empty() ? false : true;
        });
        removed(false, urls);
        respond(nullptr);
    } else if (method == "deleteAll") {
        store.ClearHistory();
        removed(true, { });
        respond(nullptr);
    } else
        return BrowserDataAnswer::Failure(BrowserDataAnswer::Status::NotSupported);
    return answer;
}

BrowserDataAnswer ExtensionBookmarksCommand(ProfileStore& store, const std::string& method, const std::string& argumentText)
{
    BrowserDataAnswer answer;
    const auto arguments = nlohmann::json::parse(argumentText.empty() ? std::string("{}") : argumentText, nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) { return BrowserDataAnswer::Failure(BrowserDataAnswer::Status::BadValue); }
    auto respond = [&](const nlohmann::json& data) { answer.data = data.dump(); };
    auto fail = [&](const char* error) {
        answer.status = BrowserDataAnswer::Status::BadValue;
        answer.data = "null";
        answer.error = error;
    };
    auto argument = [&](const char* key) { return BookmarkID(arguments.value(key, std::string())); };
    const auto id = argument("id");
    if (method == "getTree") {
        respond(nlohmann::json::array({ReadProfile(store, [](const Profile& profile) { return BookmarkRootJSON(profile.bookmarks, 64); })}));
    } else if (method == "get" || method == "getChildren" || method == "getSubTree") {
        std::vector<int64_t> ids;
        if (method == "get") {
            for (const auto& value : arguments.value("ids", nlohmann::json::array())) {
                const auto parsed = value.is_string() ? BookmarkID(value.get<std::string>()) : std::nullopt;
                if (!parsed) { fail("Can't find bookmark for id."); return answer; }
                ids.push_back(*parsed);
            }
        } else if (id)
            ids.push_back(*id);
        auto items = nlohmann::json::array();
        bool found = !ids.empty();
        store.Read([&](const Profile& profile) {
            for (auto each : ids) {
                auto item = FindBookmarkJSON(profile.bookmarks, each, method == "get" ? 0 : method == "getChildren" ? 1 : 64);
                if (!item) { found = false; return; }
                if (method == "getChildren") items = item->value("children", nlohmann::json::array());
                else items.push_back(std::move(*item));
            }
        });
        if (!found) fail("Can't find bookmark for id.");
        else respond(items);
    } else if (method == "getRecent") {
        const size_t count = static_cast<size_t>(std::clamp(arguments.value("numberOfItems", 1.0), 1.0, 100000.0));
        std::vector<std::pair<int64_t, nlohmann::json>> found;
        store.Read([&](const Profile& profile) {
            for (const auto* folder : {&profile.bookmarks.Bar(), &profile.bookmarks.Other()}) {
                EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
                    if (!node.folder && !node.separator) found.emplace_back(node.added, BookmarkJSON(node, parent, index, 0));
                });
            }
        });
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        auto items = nlohmann::json::array();
        for (size_t i = 0; i < found.size() && i < count; ++i) items.push_back(std::move(found[i].second));
        respond(items);
    } else if (method == "search") {
        // Every word of query in the title or address; url and title match exactly.
        std::vector<std::string> words;
        {
            std::istringstream stream(LowerText(arguments.value("query", std::string())));
            for (std::string word; stream >> word;) words.push_back(word);
        }
        const bool byURL = arguments.contains("url"), byTitle = arguments.contains("title");
        const std::string url = arguments.value("url", std::string()), title = arguments.value("title", std::string());
        auto items = nlohmann::json::array();
        store.Read([&](const Profile& profile) {
            for (const auto* folder : {&profile.bookmarks.Bar(), &profile.bookmarks.Other()}) {
                EachBookmark(*folder, [&](const BookmarkNode& node, int64_t parent, size_t index) {
                    if (node.separator || (byURL && (node.folder || node.url != url)) || (byTitle && node.title != title)) return;
                    if (!words.empty()) {
                        const std::string haystack = LowerText(node.title + "\n" + node.url);
                        for (const auto& word : words)
                            if (haystack.find(word) == std::string::npos) return;
                    }
                    if (words.empty() && !byURL && !byTitle) return;
                    items.push_back(BookmarkJSON(node, parent, index, 0));
                });
            }
        });
        respond(items);
    } else if (method == "create" || method == "move" || method == "update" || method == "remove" || method == "removeTree") {
        if (method != "create" && !id) { fail("Can't find bookmark for id."); return answer; }
        const char* error = nullptr;
        int64_t target = id.value_or(0);
        const size_t index = arguments.contains("index") ? static_cast<size_t>(arguments.value("index", 0.0)) : kBookmarkEnd;
        store.Change(ProfileStore::Kind::Bookmarks, [&](Profile& profile) -> bool {
            auto& bookmarks = profile.bookmarks;
            const auto* node = target ? bookmarks.Find(target) : nullptr;
            if (method != "create" && !node) { error = "Can't find bookmark for id."; return false; }
            const bool fixed = target == kBookmarksBarFolder || target == kOtherBookmarksFolder;
            if (method == "create") {
                const auto parent = arguments.contains("parentId") ? argument("parentId") : std::optional<int64_t>(kOtherBookmarksFolder);
                const auto* folder = parent ? bookmarks.Find(*parent) : nullptr;
                if (!folder || !folder->folder) { error = "Can't find parent bookmark for id."; return false; }
                const std::string title = arguments.value("title", std::string());
                if (arguments.contains("url")) {
                    const std::string url = arguments.value("url", std::string());
                    if (!IsBookmarkableURL(url)) { error = "Invalid URL."; return false; }
                    target = bookmarks.Add({url, title}, *parent, index, std::time(nullptr));
                } else
                    target = bookmarks.AddFolder(*parent, title, index, std::time(nullptr));
                if (!target) { error = "Can't create the bookmark: the folder is full."; return false; }
                return true;
            }
            if (fixed) { error = "Can't modify the root bookmark folders."; return false; }
            if (method == "move") {
                const auto parent = arguments.contains("parentId") ? argument("parentId") : std::nullopt;
                const auto* current = bookmarks.Parent(target);
                const int64_t folder = parent.value_or(current ? current->id : 0);
                const auto* destination = bookmarks.Find(folder);
                if (!destination || !destination->folder) { error = "Can't find parent bookmark for id."; return false; }
                for (int64_t inside = folder; inside; ) {
                    if (inside == target) { error = "Can't move a folder into itself."; return false; }
                    const auto* up = bookmarks.Parent(inside);
                    inside = up ? up->id : 0;
                }
                return bookmarks.Move(target, folder, index) ? true : false;
            }
            if (method == "update") {
                std::string url = arguments.value("url", node->url);
                if (node->folder && arguments.contains("url")) { error = "Can't set URL of a bookmark folder."; return false; }
                if (!node->folder && !IsBookmarkableURL(url)) { error = "Invalid URL."; return false; }
                return bookmarks.Edit(target, arguments.value("title", node->title), url) ? true : false;
            }
            if (method == "remove" && node->folder && !node->children.empty()) { error = "Can't remove non-empty folder (use recursive to force)."; return false; }
            return bookmarks.Remove(target) ? true : false;
        });
        if (error) { fail(error); return answer; }
        // Extensions hear of the change before the answer arrives, as in
        // Chrome: the host reports the changes (BookmarkChangeReporter) first.
        answer.bookmarksChanged = true;
        if (method == "remove" || method == "removeTree") { respond(nullptr); return answer; }
        auto item = ReadProfile(store, [&](const Profile& profile) { return FindBookmarkJSON(profile.bookmarks, target, 0); });
        respond(item ? *item : nlohmann::json());
    } else
        return BrowserDataAnswer::Failure(BrowserDataAnswer::Status::NotSupported);
    return answer;
}

ExtensionSearch ExtensionSearchCommand(const std::string& method, const std::string& argumentText)
{
    ExtensionSearch search;
    const auto arguments = nlohmann::json::parse(argumentText.empty() ? std::string("{}") : argumentText, nullptr, false);
    if (arguments.is_discarded() || !arguments.is_object()) {
        search.answer = BrowserDataAnswer::Failure(BrowserDataAnswer::Status::BadValue);
        return search;
    }
    auto fail = [&](const char* error) {
        search.answer.status = BrowserDataAnswer::Status::BadValue;
        search.answer.error = error;
        return search;
    };
    const auto& current = CurrentSearchEngine();
    if (method == "get") {
        auto engines = nlohmann::json::array();
        for (const auto& engine : SearchEngines()) {
            const std::string prefix = engine.prefix;
            engines.push_back({{"name", engine.name}, {"isDefault", engine.id == current.id},
                {"favIconUrl", prefix.substr(0, prefix.find('/', 8)) + "/favicon.ico"}});
        }
        search.answer.data = engines.dump();
        return search;
    }
    if (method != "query" && method != "search") {
        search.answer = BrowserDataAnswer::Failure(BrowserDataAnswer::Status::NotSupported);
        return search;
    }
    // Firefox names the engine as search.get() does; Summit's ids work too.
    const SearchEngine* engine = &current;
    if (arguments.contains("engine")) {
        const std::string name = LowerText(arguments.value("engine", std::string()));
        engine = nullptr;
        for (const auto& each : SearchEngines())
            if (name == LowerText(each.name) || name == each.id) engine = &each;
        if (!engine) return fail("No search engine with that name.");
    }
    const std::string text = Trim(arguments.value("text", std::string()));
    if (text.empty()) return fail("The search text is empty.");
    search.url = engine->prefix + PercentEncode(text);
    search.disposition = arguments.value("disposition", std::string("CURRENT_TAB"));
    return search;
}

std::string ExtensionDetectLanguageAnswer(std::string_view text)
{
    const auto detected = DetectTextLanguages(text);
    auto languages = nlohmann::json::array();
    for (const auto& [language, percentage] : detected.languages)
        languages.push_back({{"language", language}, {"percentage", percentage}});
    return nlohmann::json({{"isReliable", detected.reliable}, {"languages", languages}}).dump();
}

std::vector<BookmarkChangeReporter::Event> BookmarkChangeReporter::Changes(const Bookmarks& bookmarks)
{
    std::vector<Event> events;
    auto now = BookmarkSnapshot(bookmarks);
    if (!fReported) {
        fReported = std::move(now);
        return events;
    }
    const auto& before = *fReported;
    // Removed: only the top of a removed folder, as in Chrome.
    std::optional<std::multimap<int64_t, int64_t>> children;
    for (const auto& [id, entry] : before) {
        if (now.contains(id) || (before.contains(entry.parent) && !now.contains(entry.parent))) continue;
        if (!children) {
            children.emplace();
            for (const auto& [child, item] : before) children->emplace(item.parent, child);
        }
        events.push_back({"removed", id, nlohmann::json({{"parentId", std::to_string(entry.parent)}, {"index", entry.index},
            {"node", BookmarkTreeJSON(before, *children, id)}})});
    }
    // Moved: a new folder, or out of order with the siblings that stayed
    // (those off the longest run kept in their old order).
    std::map<int64_t, std::vector<int64_t>> stayed;
    for (const auto& [id, entry] : now) {
        auto old = before.find(id);
        if (old == before.end()) continue;
        if (old->second.parent != entry.parent)
            events.push_back({"moved", id, nlohmann::json({{"parentId", std::to_string(entry.parent)}, {"index", entry.index},
                {"oldParentId", std::to_string(old->second.parent)}, {"oldIndex", old->second.index}})});
        else
            stayed[entry.parent].push_back(id);
    }
    for (auto& [parent, ids] : stayed) {
        std::sort(ids.begin(), ids.end(), [&](int64_t a, int64_t b) { return now.at(a).index < now.at(b).index; });
        // Longest increasing run of old indexes, by patience sorting.
        std::vector<size_t> tails, previous(ids.size(), SIZE_MAX), tailAt;
        for (size_t i = 0; i < ids.size(); ++i) {
            const size_t old = before.at(ids[i]).index;
            auto at = std::lower_bound(tails.begin(), tails.end(), old) - tails.begin();
            if (at == static_cast<ptrdiff_t>(tails.size())) { tails.push_back(old); tailAt.push_back(i); }
            else { tails[at] = old; tailAt[at] = i; }
            if (at) previous[i] = tailAt[at - 1];
        }
        std::set<size_t> kept;
        for (size_t i = tailAt.empty() ? SIZE_MAX : tailAt.back(); i != SIZE_MAX; i = previous[i]) kept.insert(i);
        for (size_t i = 0; i < ids.size(); ++i) {
            if (kept.contains(i)) continue;
            const auto& entry = now.at(ids[i]);
            events.push_back({"moved", ids[i], nlohmann::json({{"parentId", std::to_string(parent)}, {"index", entry.index},
                {"oldParentId", std::to_string(parent)}, {"oldIndex", before.at(ids[i]).index}})});
        }
    }
    for (const auto& [id, entry] : now) {
        auto old = before.find(id);
        if (old == before.end()) {
            events.push_back({"created", id, entry.node});
        } else if (old->second.title != entry.title || old->second.url != entry.url) {
            nlohmann::json info = {{"title", entry.title}};
            if (!entry.url.empty()) info["url"] = entry.url;
            events.push_back({"changed", id, std::move(info)});
        }
    }
    fReported = std::move(now);
    return events;
}

} // namespace summit
