#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <unistd.h>
using namespace summit;
using nlohmann::json;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static json Tree(const std::vector<BookmarkNode>& nodes)
{
    auto result = json::array();
    for (const auto& node : nodes) {
        CHECK(node.id == 0);
        if (node.folder) {
            CHECK(node.url.empty());
            result.push_back({{"title",node.title},{"children",Tree(node.children)}});
        } else result.push_back({{"title",node.title},{"url",node.url}});
    }
    return result;
}
static const BookmarkNode* Group(const BookmarkQueryResult& result, const std::string& title)
{
    for (const auto& node : result.items) if (node.title == title && node.folder) return &node;
    return nullptr;
}
static std::string Titles(const std::vector<BookmarkNode>& nodes)
{
    std::string text;
    for (const auto& node : nodes) text += (text.empty() ? "" : ",") + node.title;
    return text;
}
static void CanonicalizeEqualVisitRanks(json& nodes, const std::map<std::string,std::pair<int,int64_t>>& ranks)
{
    // Places' visit-count comparator returns equality when both count and last
    // visit match. Its SQL row order is unspecified there. Normalize only each
    // consecutive equal-rank run, preserving every order between unequal ranks.
    for (auto& node : nodes)
        if (node.contains("children")) CanonicalizeEqualVisitRanks(node["children"],ranks);
    for (size_t first = 0; first < nodes.size();) {
        if (!nodes[first].contains("url")) { ++first; continue; }
        const auto rank = ranks.at(nodes[first]["url"]);
        size_t end = first + 1;
        while (end < nodes.size() && nodes[end].contains("url") && ranks.at(nodes[end]["url"]) == rank) ++end;
        std::sort(nodes.begin()+first,nodes.begin()+end,[](const auto& a, const auto& b) { return a["url"] < b["url"]; });
        first = end;
    }
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const bool hadTZ = std::getenv("TZ");
    const std::string originalTZ = hadTZ ? std::getenv("TZ") : "";
    json fixture; std::ifstream(argv[1]) >> fixture;
    setenv("TZ",fixture["timezone"].get<std::string>().c_str(),1); tzset();
    Profile profile;
    auto add = [&](std::string title, std::vector<int64_t> times) {
        PageRecord page{"https://date.test/"+std::to_string(profile.history.size()),std::move(title),0,false,int(times.size())};
        SetHistoryVisitTimes(page,std::move(times)); profile.history.push_back(std::move(page));
    };
    std::map<std::string,std::pair<int,int64_t>> ranks;
    for (const auto& seed : fixture["seed"]) {
        PageRecord page{seed["url"],seed["title"],0,false,int(seed["times"].size())};
        SetHistoryVisitTimes(page,seed["times"].get<std::vector<int64_t>>());
        ranks[page.url] = {page.visits,page.visitTimes.front()};
        profile.history.push_back(std::move(page));
    }
    int64_t now = fixture["now"];
    auto query = [&](const std::string& url) { return QueryBookmarks(url,profile.bookmarks,profile.history,{},now); };
    for (const auto& test : fixture["queries"]) {
        const auto result = query(test["query"]);
        CHECK(result.error.empty());
        auto actual = Tree(result.items);
        auto expected = test["groups"];
        const auto url = test["query"].get<std::string>();
        if (url.find("place:type=3&sort=7") == 0 || url.find("place:type=3&sort=8") == 0) {
            CanonicalizeEqualVisitRanks(actual,ranks); CanonicalizeEqualVisitRanks(expected,ranks);
        }
        if (actual != expected) {
            std::cerr << "Firefox oracle mismatch: " << test["query"] << '\n';
            std::cerr << "Actual: " << actual.dump() << '\n' << "Expected: " << expected.dump() << '\n';
            CHECK(false);
        } else CHECK(true);
    }
    for (const auto type : {3,5}) {
        for (const auto* suffix : {"&terms=Today","&beginTime=1","&domain=date.test","&tag=work","&onlyBookmarked=1",
                "&minVisits=1","&parent=toolbar_____","&OR","&transition=1","&includeHidden=1"}) {
            const auto result = query("place:type="+std::to_string(type)+suffix);
            CHECK(!result.error.empty() && result.items.empty());
        }
    }
    constexpr int64_t second = 1000000;
    // DST days are calendar days: spring has 23 hours, autumn has 25.
    for (const auto today : {1710046800000000LL,1730606400000000LL}) {
        profile.history.clear(); now = today + 12*3600*second;
        const auto tomorrow = today + (today == 1710046800000000LL ? 23 : 25)*3600*second;
        add("A midnight",{today}); add("B before midnight",{today-1});
        add("C last moment",{tomorrow-1}); add("D next midnight",{tomorrow});
        auto result = query("place:type=3");
        const auto* current = Group(result,"Today"); const auto* yesterday = Group(result,"Yesterday");
        CHECK(current && Titles(current->children) == "A midnight,C last moment,D next midnight");
        CHECK(yesterday && Titles(yesterday->children) == "A midnight,B before midnight");
        CHECK(!Group(result,"Last 7 days") && !Group(result,"This month"));
        profile.history.clear(); add("Only future boundary",{tomorrow});
        CHECK(query("place:type=3").items.empty()); // Inclusive membership does not make the folder visible.
    }
    setenv("TZ","UTC",1); tzset();
    now = 1704110400000000; // 1 January 2024 noon.
    profile.history.clear();
    add("Today",{1704067200000000}); add("Prior year",{1703980800000000});
    add("Week start",{1703462400000000}); add("Before week",{1703462399999999});
    add("December start",{1701388800000000}); add("November last",{1701388799999999});
    auto result = query("place:type=3");
    CHECK(Titles(result.items) == "Today,Yesterday,Last 7 days,December 2023,November 2023");
    const auto* week = Group(result,"Last 7 days");
    CHECK(week && Titles(week->children) == "Prior year,Today,Week start");
    const auto* previous = Group(result,"December 2023");
    CHECK(previous && previous->children.size() == 5); // Includes 1 January's exact boundary.
    const auto* november = Group(result,"November 2023");
    CHECK(november && Titles(november->children) == "December start,November last");
    result = query("place:type=3&sort=2&maxResults=2");
    CHECK(Titles(result.items) == "Yesterday,Today");
    result = query("place:type=5&sort=8&maxResults=1");
    CHECK(result.items.size() == 1 && result.items[0].title == "Today" && result.items[0].children.size() == 1
        && result.items[0].children[0].title == "date.test");
    now = 1709294400000000; // 1 March 2024 noon, after leap day.
    profile.history.clear(); add("Leap day",{1709164800000000});
    result = query("place:type=3");
    CHECK(Titles(result.items) == "Yesterday,February");
    CHECK(Group(result,"February") && Group(result,"February")->children.size() == 1);
    // Half-open visibility versus inclusive contents at the older-month boundary.
    now = 1792584000000000; // 21 October 2026 noon.
    profile.history.clear();
    add("Before oldest",{1969999999}); add("At oldest",{1970000000});
    add("May boundary",{1777593600000000}); add("April end",{1777593599999999});
    result = query("place:type=3");
    CHECK(Titles(result.items) == "May,Older than 6 months");
    const auto* older = Group(result,"Older than 6 months");
    CHECK(older && Titles(older->children) == "April end,At oldest,May boundary");
    // Fractional timezone offsets, unknown dates and local legacy second values.
    setenv("TZ","Asia/Kathmandu",1); tzset();
    now = 1700000000000000;
    std::time_t seconds = now / second; std::tm local {}; localtime_r(&seconds,&local);
    local.tm_hour = local.tm_min = local.tm_sec = 0; local.tm_isdst = -1;
    const auto midnight = int64_t(std::mktime(&local))*second;
    profile.history = {{"https://legacy.test/","Legacy",midnight/second,false,1},{"https://unknown.test/","Unknown"}};
    result = query("place:type=3");
    CHECK(Titles(result.items) == "Today" && result.items[0].children[0].title == "Legacy");
    now = INT64_MAX;
    CHECK(!query("place:type=3").error.empty() && query("place:type=3").items.empty());
    now = 1700000000000000;
    const auto id = profile.bookmarks.Add({"place:type=5&sort=4", "History by date and site"},kBookmarksBarFolder);
    // Rendering uses the real current time; the old legacy visit belongs to Older.
    const auto page = RenderBookmarkQueryPage(profile.bookmarks,profile.history,id,{},"test");
    CHECK(page.find("<h2>Older than 6 months</h2>") != std::string::npos && page.find("<h3>legacy.test</h3>") != std::string::npos);
    CHECK(profile.bookmarks.Find(id)->children.empty() && profile.bookmarks.Count() == 1);
    const auto directory = std::filesystem::temp_directory_path() / ("summit-date-queries-"+std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    std::string error;
    CHECK(profile.Save(directory/"profile.json",error));
    auto saved = Profile::Load(directory/"profile.json",error);
    CHECK(error.empty() && saved.bookmarks.Find(id)->children.empty()
        && Tree(QueryBookmarks("place:type=5",saved.bookmarks,saved.history,{},now).items) == Tree(query("place:type=5").items));
    BookmarkNode html;
    CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(profile.bookmarks),html,error));
    Bookmarks restored;
    CHECK(restored.Import(html,"HTML").added == 1 && restored.FindURL("place:type=5&sort=4"));
    profile.history.clear();
    CHECK(query("place:type=5").items.empty() && profile.bookmarks.Count() == 1);
    std::filesystem::remove_all(directory);
    if (hadTZ) setenv("TZ",originalTZ.c_str(),1); else unsetenv("TZ"); tzset();
    std::cout << checks << " date query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
