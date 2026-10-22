#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
using namespace summit;
using nlohmann::json;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
using Row = std::tuple<std::string,std::string,int64_t>;
static std::vector<Row> Rows(const BookmarkQueryResult& result)
{
    CHECK(result.error.empty());
    CHECK(result.items.size() == result.visitTimes.size());
    std::vector<Row> rows;
    for (size_t i = 0; i < result.items.size() && i < result.visitTimes.size(); ++i) {
        const auto& node = result.items[i];
        CHECK(!node.id && !node.folder && !node.separator && node.children.empty() && !node.added && !node.modified);
        rows.emplace_back(node.title,node.url,result.visitTimes[i]);
    }
    CHECK(std::set<Row>(rows.begin(),rows.end()).size() == rows.size());
    return rows;
}
static int Option(const std::string& url, const std::string& option)
{
    auto start = url.find(option+"=");
    return start == std::string::npos ? 0 : std::stoi(url.substr(start+option.size()+1));
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const bool hadTZ = std::getenv("TZ");
    const std::string originalTZ = hadTZ ? std::getenv("TZ") : "";
    setenv("TZ","UTC",1); tzset();
    json fixture; std::ifstream(argv[1]) >> fixture;
    Profile profile;
    std::map<std::string,int> counts;
    for (const auto& seed : fixture["seed"]) {
        PageRecord page{seed["url"],seed["title"],0,false,int(seed["times"].size())};
        SetHistoryVisitTimes(page,seed["times"].get<std::vector<int64_t>>());
        counts[page.url] = page.visits;
        profile.history.push_back(std::move(page));
    }
    for (const auto& [url,tags] : fixture["tags"].items()) {
        auto id = profile.bookmarks.Add({url,"Saved "+url},kBookmarksBarFolder);
        BookmarkMetadata metadata; metadata.tags = tags.get<std::vector<std::string>>();
        profile.bookmarks.SetMetadata(id,metadata);
    }
    int64_t now = fixture["now"];
    auto query = [&](const std::string& url) { return QueryBookmarks(url,profile.bookmarks,profile.history,{},now); };
    const auto all = Rows(query("place:type=1"));
    for (const auto& test : fixture["queries"]) {
        const auto url = test["uri"].get<std::string>();
        if (url.find("parent=") != std::string::npos) {
            const auto result = query(url);
            CHECK(!result.error.empty() && result.items.empty() && result.visitTimes.empty());
            continue;
        }
        if (url.find("onlyBookmarked=") != std::string::npos) continue; // Removed in current Firefox; legacy behavior below.
        auto actual = Rows(query(url));
        std::vector<Row> expected;
        for (const auto& row : test["tree"]["children"])
            expected.emplace_back(row["title"],row["uri"],row["time"]);
        CHECK(actual.size() == expected.size());
        const int mode = Option(url,"sort"), limit = Option(url,"maxResults");
        // SQLite has no ORDER BY for NONE or visit metadata sorts. With LIMIT,
        // any distinct retained subset is legal; post-sort must still be right.
        if (limit && (!mode || mode >= 11)) {
            for (const auto& row : actual) CHECK(std::find(all.begin(),all.end(),row) != all.end());
            auto sorted = query(url.substr(0,url.find("&maxResults=")));
            const auto full = Rows(sorted);
            std::vector<Row> subsequence;
            for (const auto& row : full) if (std::find(actual.begin(),actual.end(),row) != actual.end()) subsequence.push_back(row);
            if (mode == 11 || mode == 12 || mode == 13 || mode == 14) {
                // Timestamp order within equal title ranks is unspecified.
                std::vector<std::string> a,b;
                for (const auto& row : actual) a.push_back(std::get<0>(row));
                for (const auto& row : subsequence) b.push_back(std::get<0>(row));
                CHECK(a == b);
            } else if (mode) CHECK(actual == subsequence);
            continue;
        }
        if (limit && (mode == 5 || mode == 6 || mode == 7 || mode == 8)) {
            // SQL limits on URL/count before the in-memory timestamp tie-break.
            // Preserve every primary rank, validate the chosen rows and tie order.
            std::vector<json> a,b;
            auto rank = [&](const Row& row) -> json { return mode <= 6 ? json(std::get<1>(row)) : json(counts.at(std::get<1>(row))); };
            for (const auto& row : actual) { a.push_back(rank(row)); CHECK(std::find(all.begin(),all.end(),row) != all.end()); }
            for (const auto& row : expected) b.push_back(rank(row));
            CHECK(a == b);
            for (size_t i = 1; i < actual.size(); ++i)
                if (rank(actual[i-1]) == rank(actual[i]))
                    CHECK(mode % 2 ? std::get<2>(actual[i-1]) <= std::get<2>(actual[i]) : std::get<2>(actual[i-1]) >= std::get<2>(actual[i]));
            continue;
        }
        if (!mode) { std::sort(actual.begin(),actual.end()); std::sort(expected.begin(),expected.end()); }
        if (mode >= 11 && mode <= 14) {
            auto canonicalize = [](auto& rows) {
                for (size_t first = 0; first < rows.size();) {
                    size_t end = first+1;
                    while (end < rows.size() && std::get<0>(rows[end]) == std::get<0>(rows[first])) ++end;
                    std::sort(rows.begin()+first,rows.begin()+end); first = end;
                }
            };
            canonicalize(actual); canonicalize(expected);
        }
        if (actual != expected) { std::cerr << "Firefox mismatch: " << url << '\n'; CHECK(false); }
        else CHECK(true);
    }
    const auto& a = profile.history[0];
    const auto times = HistoryVisitTimes(a);
    const auto exact = "&beginTime="+std::to_string(times[1])+"&endTime="+std::to_string(times[1]);
    auto result = query("place:type=1"+exact+"&sort=4");
    CHECK(result.items.size() == 3 && result.visitTimes == times); // Page-level EXISTS, not per-row range filtering.
    CHECK(query("place:type=1"+exact+"&maxVisits=2").items.empty());
    CHECK(query("place:type=1&onlyBookmarked=1").items.size() == 5);
    result = query("place:type=1&uri=http%3A%2F%2Falpha.example%2Fa&OR&domain=alpha.example&sort=4");
    CHECK(result.items.size() == 5 && Rows(result).size() == 5); // Overlap does not duplicate a visit.
    CHECK(query("place:type=1&OR&uri=http%3A%2F%2Falpha.example%2Fa").items.size() == 3);
    CHECK(query("place:type=1&beginTimeRef=2&beginTime="+std::to_string(times[1]-now)+"&endTimeRef=2&endTime="+std::to_string(times[1]-now)).items.size() == 3);
    for (const auto* suffix : {"&parent=toolbar_____","&folder=TOOLBAR","&transition=invalid","&annotation=x","&sort=22","&includeHidden=1","&beginTimeRef=2&beginTime=9223372036854775807"}) {
        const auto bad = query(std::string("place:type=1")+suffix);
        CHECK(!bad.error.empty() && bad.items.empty() && bad.visitTimes.empty());
    }
    CHECK(query("place:type=0").visitTimes.empty());
    const auto id = profile.bookmarks.Add({"place:type=1&sort=4","Individual visits"},kBookmarksBarFolder);
    auto page = RenderBookmarkQueryPage(profile.bookmarks,profile.history,id,{},"visit-tests");
    CHECK(page.find("class=\"visit-time\">Visited "+FormatBookmarkVisitTime(times[1])) != std::string::npos);
    CHECK(page.find("href=\"http://alpha.example/a\"") != std::string::npos);
    CHECK(profile.bookmarks.Find(id)->children.empty() && profile.bookmarks.Find(id)->title == "Individual visits");
    const auto html = WriteNetscapeBookmarks(profile.bookmarks);
    CHECK(html.find("visit-time") == std::string::npos && html.find("place:type=1&amp;sort=4") != std::string::npos);
    CHECK(FormatBookmarkVisitTime(1704067200123456) == "2024-01-01 00:00:00.123456 +0000");
    CHECK(FormatBookmarkVisitTime(1704067200000000) == "2024-01-01 00:00:00 +0000");
    CHECK(FormatBookmarkVisitTime(-1) == "1969-12-31 23:59:59.999999 +0000");
    setenv("TZ","America/New_York",1); tzset();
    CHECK(FormatBookmarkVisitTime(1730611800000000) == "2024-11-03 01:30:00 -0400");
    CHECK(FormatBookmarkVisitTime(1730615400000000) == "2024-11-03 01:30:00 -0500");
    setenv("TZ","Asia/Kathmandu",1); tzset();
    CHECK(FormatBookmarkVisitTime(1704067200123456) == "2024-01-01 05:45:00.123456 +0545");
    setenv("TZ","America/St_Johns",1); tzset();
    CHECK(FormatBookmarkVisitTime(1704067200000000) == "2023-12-31 20:30:00 -0330");
    setenv("TZ","UTC",1); tzset();
    profile.history = {{"https://retention.test/","Repeated",0,false,10000}};
    std::vector<int64_t> many;
    for (int i = 0; i < 400; ++i) many.push_back(1704067200000000LL+i*100LL);
    SetHistoryVisitTimes(profile.history[0],many);
    result = query("place:type=1&sort=4&minVisits=10000");
    CHECK(result.items.size() == kRecentHistoryVisits && result.visitTimes.front() == many.back());
    CHECK(query("place:type=1&sort=3&maxResults=1").visitTimes[0] == result.visitTimes.back());
    profile.history = {{"https://legacy.test/","Legacy",1704067200,false,10000}};
    profile.history[0].earlier = {1703980800};
    CHECK(query("place:type=1&sort=4").visitTimes == std::vector<int64_t>({1704067200000000LL,1703980800000000LL}));
    SetHistoryVisitTimes(profile.history[0],{1704067200000123LL,1704067200000123LL,1704067200000456LL});
    CHECK(query("place:type=1").items.size() == 2); // Exact samples, no rounded phantom row.
    profile.history.push_back({"https://unknown.test/","Unknown",0,false,99});
    CHECK(query("place:type=1").items.size() == 2); // Aggregate count never fabricates timestamps.
    profile.history.clear();
    CHECK(query("place:type=1").items.empty() && query("place:type=1").visitTimes.empty());
    CHECK(RenderBookmarkQueryPage(profile.bookmarks,profile.history,id,{},"visit-tests").find("No matching pages.") != std::string::npos);
    if (hadTZ) setenv("TZ",originalTZ.c_str(),1); else unsetenv("TZ"); tzset();
    std::cout << checks << " visit query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
