#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <unistd.h>
using namespace summit;
using nlohmann::json;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static json Tree(const BookmarkQueryResult& result)
{
    auto tree = json::array();
    for (const auto& group : result.items) {
        auto children = json::array();
        CHECK(group.folder && group.id == 0 && group.url.empty());
        for (const auto& child : group.children) {
            CHECK(!child.folder && child.id == 0);
            children.push_back({{"title",child.title},{"url",child.url}});
        }
        tree.push_back({{"title",group.title},{"children",children}});
    }
    return tree;
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    json fixture; std::ifstream(argv[1]) >> fixture;
    Profile profile;
    for (const auto& seed : fixture["seed"]) {
        PageRecord page{seed["url"],seed["title"],0,false,int(seed["times"].size())};
        SetHistoryVisitTimes(page,seed["times"].get<std::vector<int64_t>>());
        profile.history.push_back(std::move(page));
    }
    const int64_t now = fixture["now"];
    auto query = [&](const std::string& url) { return QueryBookmarks(url,profile.bookmarks,profile.history,{},now); };
    for (const auto& test : fixture["queries"]) {
        const std::string url = test["query"];
        const auto result = query(url);
        CHECK(result.error.empty());
        if (Tree(result) != test["groups"]) {
            std::cerr << "Firefox oracle mismatch: " << url << '\n';
            CHECK(false);
        } else CHECK(true);
    }
    // Unsupported filters are preserved with an explanation, never broadened.
    for (const auto* url : {"place:type=4&terms=Alpha", "place:type=4&onlyBookmarked=1", "place:type=4&minVisits=2",
            "place:type=4&tag=work", "place:type=4&parent=toolbar_____", "place:type=4&OR",
            "place:type=4&beginTime=1", "place:type=4&endTime=2", "place:type=4&domain=alpha.example",
            "place:type=4&uri=https%3A%2F%2Falpha.example", "place:type=4&transition=1", "place:type=4&sort=22",
            "place:type=4&beginTimeRef=2&beginTime=9223372036854775807&endTime=1"}) {
        const auto result = query(url);
        CHECK(!result.error.empty() && result.items.empty());
    }
    auto bounded = query("place:type=4&beginTimeRef=2&beginTime=-1&endTimeRef=2&endTime=1");
    CHECK(bounded.error.empty() && bounded.items.empty());
    CHECK(query("place:type=4&beginTime=10&endTime=1").items.empty());
    const auto oldVisit = profile.history[3].visitTimes.back();
    bounded = query("place:type=4&beginTime="+std::to_string(oldVisit)+"&endTime="+std::to_string(oldVisit));
    CHECK(bounded.error.empty() && bounded.items.size() == 1 && bounded.items[0].children.size() == 1
        && bounded.items[0].children[0].title == "Dual");
    // Exact hosts, independent of scheme, credentials and port; subdomains stay separate.
    profile.history = {{"https://HOST.test:8443/a","B",10,false,1}, {"http://user@host.test/b","A",20,false,2},
        {"https://sub.host.test/","Sub",30,false,1}, {"http://[::1]:8080/","IPv6",40,false,1},
        {"file:///tmp/a","Local",50,false,1}, {"javascript:void(0)","Script",60,false,1}};
    auto result = query("place:type=4&sort=1");
    CHECK(result.error.empty() && result.items.size() == 4);
    CHECK(result.items[2].title == "host.test" && result.items[2].children.size() == 2
        && result.items[2].children[0].title == "A");
    auto limited = query("place:type=4&sort=2&maxResults=1");
    CHECK(limited.items.size() == 1 && limited.items[0].title == "(local files)");
    limited = query("place:type=4&sort=6&maxResults=1");
    CHECK(limited.items.size() == 1 && limited.items[0].title == "sub.host.test");
    const auto queryID = profile.bookmarks.Add({"place:type=4&sort=1", "History by site"},kBookmarksBarFolder);
    const auto savedCount = profile.bookmarks.Count();
    const auto rendered = RenderBookmarkQueryPage(profile.bookmarks,profile.history,queryID,{},"test");
    CHECK(rendered.find("host.test</h2>") != std::string::npos && rendered.find("https://HOST.test:8443/a") != std::string::npos);
    CHECK(profile.bookmarks.Count() == savedCount && profile.bookmarks.Find(queryID)->children.empty());
    const auto dir = std::filesystem::temp_directory_path() / ("summit-site-queries-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    std::string error;
    CHECK(profile.Save(dir/"profile.json",error));
    auto loaded = Profile::Load(dir/"profile.json",error);
    CHECK(error.empty() && loaded.bookmarks.Count() == savedCount && loaded.bookmarks.Find(queryID)->children.empty());
    CHECK(Tree(QueryBookmarks("place:type=4&sort=1",loaded.bookmarks,loaded.history)) == Tree(result));
    BookmarkNode imported;
    CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(profile.bookmarks),imported,error));
    Bookmarks roundtrip;
    CHECK(roundtrip.Import(imported,"HTML").added == 1 && roundtrip.FindURL("place:type=4&sort=1"));
    profile.Visit({"https://new.test/","New site"},70);
    CHECK(query("place:type=4").items.size() == 5 && result.items.size() == 4);
    profile.history.clear();
    CHECK(query("place:type=4").items.empty() && profile.bookmarks.Count() == savedCount);
    profile.history = {{"https://ties.test/z","",1,false,1},{"https://ties.test/b","Same",1,false,1},
        {"https://ties.test/a","Same",1,false,1}};
    result = query("place:type=4&sort=1");
    CHECK(result.items[0].children[0].url == "https://ties.test/z" && result.items[0].children[1].url == "https://ties.test/a");
    result = query("place:type=4&sort=2");
    CHECK(result.items[0].children[0].url == "https://ties.test/b" && result.items[0].children[2].url == "https://ties.test/z");
    std::filesystem::remove_all(dir);
    std::cout << checks << " site query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
