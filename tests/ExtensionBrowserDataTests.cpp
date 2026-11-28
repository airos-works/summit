// core/ExtensionBrowserData: chrome.history, chrome.bookmarks and the bookmark
// events, as air/OS and Android answer extensions.
#include "core/ExtensionBrowserData.h"
#include "core/HistoryVisits.h"
#include <iostream>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)

using namespace summit;
using Status = BrowserDataAnswer::Status;

namespace {

class MemoryStore final : public ProfileStore {
public:
    void Read(const std::function<void(const Profile&)>& function) const override { function(profile); }
    void Change(Kind kind, const std::function<bool(Profile&)>& function) override
    {
        if (function(profile))
            ++(kind == Kind::History ? historyChanges : bookmarkChanges);
    }
    void ClearHistory() override { profile.history.clear(); }

    Profile profile;
    int historyChanges = 0, bookmarkChanges = 0;
};

nlohmann::json Data(const BrowserDataAnswer& answer)
{
    return nlohmann::json::parse(answer.data);
}

} // namespace

int main()
{
    MemoryStore store;
    BookmarkChangeReporter reporter;
    CHECK(reporter.Changes(store.profile.bookmarks).empty());

    // Bookmarks: the tree, creating, finding, changing, moving and removing.
    auto tree = Data(ExtensionBookmarksCommand(store, "getTree", "{}"));
    CHECK(tree.size() == 1 && tree[0]["id"] == "0" && tree[0]["children"].size() == 2);
    CHECK(tree[0]["children"][0]["folderType"] == "bookmarks-bar");

    auto created = ExtensionBookmarksCommand(store, "create", R"({"title": "Summit", "url": "https://airos.works/summit"})");
    CHECK(created.status == Status::Ok && created.bookmarksChanged && store.bookmarkChanges == 1);
    const std::string id = Data(created)["id"];
    CHECK(Data(created)["parentId"] == "2" && Data(created)["url"] == "https://airos.works/summit");
    auto events = reporter.Changes(store.profile.bookmarks);
    CHECK(events.size() == 1 && std::string(events[0].kind) == "created" && std::to_string(events[0].id) == id);

    auto found = Data(ExtensionBookmarksCommand(store, "search", R"({"query": "summit"})"));
    CHECK(found.size() == 1 && found[0]["id"] == id);
    CHECK(Data(ExtensionBookmarksCommand(store, "search", R"({"query": "nothing"})")).empty());

    auto folder = Data(ExtensionBookmarksCommand(store, "create", R"({"parentId": "1", "title": "Work"})"));
    CHECK(folder["type"] == "folder" && folder["parentId"] == "1");
    reporter.Changes(store.profile.bookmarks);

    auto updated = ExtensionBookmarksCommand(store, "update", nlohmann::json({{"id", id}, {"title", "Summit browser"}}).dump());
    CHECK(updated.status == Status::Ok && Data(updated)["title"] == "Summit browser");
    events = reporter.Changes(store.profile.bookmarks);
    CHECK(events.size() == 1 && std::string(events[0].kind) == "changed" && events[0].info["title"] == "Summit browser");

    auto moved = ExtensionBookmarksCommand(store, "move", nlohmann::json({{"id", id}, {"parentId", folder["id"]}}).dump());
    CHECK(moved.status == Status::Ok && Data(moved)["parentId"] == folder["id"]);
    events = reporter.Changes(store.profile.bookmarks);
    CHECK(events.size() == 1 && std::string(events[0].kind) == "moved" && events[0].info["oldParentId"] == "2");

    // A non-empty folder is removed only with removeTree; it is reported once, with its children.
    auto refused = ExtensionBookmarksCommand(store, "remove", nlohmann::json({{"id", folder["id"]}}).dump());
    CHECK(refused.status == Status::BadValue && !refused.error.empty());
    CHECK(ExtensionBookmarksCommand(store, "removeTree", nlohmann::json({{"id", folder["id"]}}).dump()).status == Status::Ok);
    events = reporter.Changes(store.profile.bookmarks);
    CHECK(events.size() == 1 && std::string(events[0].kind) == "removed" && events[0].info["node"]["children"].size() == 1);

    CHECK(ExtensionBookmarksCommand(store, "update", R"({"id": "1", "title": "x"})").status == Status::BadValue);
    CHECK(ExtensionBookmarksCommand(store, "get", R"({"ids": ["999999"]})").status == Status::BadValue);
    CHECK(ExtensionBookmarksCommand(store, "create", R"({"url": "ftp://example.com/"})").status == Status::BadValue);
    CHECK(ExtensionBookmarksCommand(store, "nonsense", "{}").status == Status::NotSupported);
    CHECK(ExtensionBookmarksCommand(store, "getTree", "[").status == Status::BadValue);

    // History: adding, searching, visits and removing, with their events.
    const double now = HistoryNowMicroseconds() / 1000.0;
    auto added = ExtensionHistoryCommand(store, "addUrl", nlohmann::json({{"url", "https://example.com/"}, {"title", "Example"}, {"visitTime", now}}).dump());
    CHECK(added.status == Status::Ok && added.historyEvents.size() == 1 && added.historyEvents[0].first == "visited");
    CHECK(store.historyChanges == 1);
    auto searched = Data(ExtensionHistoryCommand(store, "search", R"({"text": "EXAMPLE"})"));
    CHECK(searched.size() == 1 && searched[0]["url"] == "https://example.com/" && searched[0]["title"] == "Example");
    CHECK(!Data(ExtensionHistoryCommand(store, "getVisits", R"({"url": "https://example.com/"})")).empty());
    auto deleted = ExtensionHistoryCommand(store, "deleteUrl", R"({"url": "https://example.com/"})");
    CHECK(deleted.historyEvents.size() == 1 && deleted.historyEvents[0].first == "removed");
    CHECK(Data(ExtensionHistoryCommand(store, "search", R"({"text": ""})")).empty());
    ExtensionHistoryCommand(store, "addUrl", R"({"url": "https://example.org/"})");
    auto cleared = ExtensionHistoryCommand(store, "deleteAll", "{}");
    CHECK(store.profile.history.empty() && cleared.historyEvents.size() == 1
        && nlohmann::json::parse(cleared.historyEvents[0].second)["allHistory"] == true);
    CHECK(ExtensionHistoryCommand(store, "addUrl", R"({"url": ""})").status == Status::BadValue);

    // Search: the engines, and where a query goes.
    auto engines = nlohmann::json::parse(ExtensionSearchCommand("get", "{}").answer.data);
    CHECK(engines.is_array() && !engines.empty());
    int defaults = 0;
    for (const auto& engine : engines) defaults += engine.value("isDefault", false);
    CHECK(defaults == 1);
    auto query = ExtensionSearchCommand("query", R"({"text": "  summit browser "})");
    CHECK(query.answer.status == Status::Ok && query.disposition == "CURRENT_TAB"
        && query.url.find("summit") != std::string::npos && query.url.find(' ') == std::string::npos);
    CHECK(ExtensionSearchCommand("search", R"({"text": "x", "disposition": "NEW_TAB"})").disposition == "NEW_TAB");
    CHECK(ExtensionSearchCommand("query", R"({"text": "   "})").answer.status == Status::BadValue);
    CHECK(ExtensionSearchCommand("query", R"({"text": "x", "engine": "no such engine"})").answer.status == Status::BadValue);
    CHECK(ExtensionSearchCommand("other", "{}").answer.status == Status::NotSupported);

    // i18n.detectLanguage.
    auto detected = nlohmann::json::parse(ExtensionDetectLanguageAnswer(
        "The quick brown fox jumps over the lazy dog while the browser loads the page in the background."));
    CHECK(detected.contains("isReliable") && !detected["languages"].empty() && detected["languages"][0]["language"] == "en");

    std::cout << checks - failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
