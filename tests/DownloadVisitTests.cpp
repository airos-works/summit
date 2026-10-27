#include "core/Profile.h"
#include "core/HistoryVisits.h"
#include "core/BookmarkQueries.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
int main()
{
    constexpr int64_t start = 1792576800123000;
    const std::string url = "https://download-oracle.test/original.zip";
    Profile profile;
    CHECK(profile.VisitDownload({url, ""}, start));
    CHECK(profile.history.size() == 1);
    CHECK(profile.history[0].visitCountKnown && HistoryVisitCount(profile.history[0]) == 0);
    CHECK(profile.history[0].visitTimes == std::vector<int64_t>{start});
    CHECK((profile.history[0].visitTransitions == std::vector<std::pair<int64_t,uint32_t>>{{start,7}}));
    CHECK(QueryBookmarks("place:transition=7&maxVisits=0", {}, profile.history).items.size() == 1);
    CHECK(QueryBookmarks("place:transition=1", {}, profile.history).items.empty());
    CHECK(!profile.SetDownloadTitle(url, start + 1, "Wrong transfer"));
    CHECK(!profile.SetDownloadTitle(url, start, ""));
    CHECK(profile.SetDownloadTitle(url, start, "archive.zip"));
    CHECK(!profile.SetDownloadTitle(url, start, "Another title"));
    CHECK(profile.history[0].title == "archive.zip");
    CHECK(profile.VisitDownload({url, "Second transfer"}, start + 10));
    CHECK(profile.history[0].title == "archive.zip" && HistoryVisitCount(profile.history[0]) == 0);
    CHECK(profile.history[0].visitTimes.size() == 2 && profile.history[0].visitTransitions.size() == 2);
    CHECK(QueryBookmarks("place:type=1&transition=7", {}, profile.history).items.size() == 2);
    CHECK(profile.Visit({url, "Page title"}, start / 1000000 + 1));
    CHECK(HistoryVisitCount(profile.history[0]) == 1);
    CHECK(profile.VisitDownload({url, "New filename"}, start + 2000000));
    CHECK(profile.history[0].title == "Page title" && HistoryVisitCount(profile.history[0]) == 1);
    CHECK(profile.history[0].visitTimes.size() == 4 && profile.history[0].visitTransitions.size() == 3);
    CHECK(profile.ForgetVisitsSince(start / 1000000 + 2) == 1);
    CHECK(HistoryVisitCount(profile.history[0]) == 1);
    CHECK(profile.ForgetVisitsSince(start / 1000000 + 1) == 1);
    CHECK(HistoryVisitCount(profile.history[0]) == 0 && profile.history[0].visitTimes.size() == 2);
    const auto path = std::filesystem::temp_directory_path() / ("summit-download-visits-" + std::to_string(getpid()));
    std::filesystem::create_directory(path);
    std::string error;
    CHECK(profile.Save(path / "profile.json", error));
    const auto loaded = Profile::Load(path / "profile.json", error);
    CHECK(error.empty() && loaded.history.size() == 1);
    CHECK(loaded.history[0].visitTransitions == profile.history[0].visitTransitions);
    CHECK(loaded.history[0].visitCountKnown && HistoryVisitCount(loaded.history[0]) == 0);
    std::filesystem::remove_all(path);
    profile.history.clear();
    CHECK(!profile.SetDownloadTitle(url, start, "Cleared transfer"));
    CHECK(profile.history.empty());
    CHECK(!profile.VisitDownload({url,"bad time"}, 0));
    CHECK(!profile.VisitDownload({url,"bad time"}, -1));
    for (const auto& address : {"data:text/plain,test", "blob:https://example.test/id", "file:///tmp/download", "not a URL"})
        CHECK(!profile.VisitDownload({address,"excluded"}, start));
    CHECK(profile.history.empty());
    PageRecord unknown{url,"Legacy title"};
    profile.history = {unknown};
    CHECK(profile.VisitDownload({url,"Filename"}, start));
    CHECK(profile.history[0].title == "Legacy title" && HistoryVisitCount(profile.history[0]) == 1);
    Profile retained;
    for (int i = 0; i < 2100; ++i) retained.VisitDownload({url,"archive.zip"}, start + i);
    CHECK(retained.history.size() == 1 && HistoryVisitCount(retained.history[0]) == 0);
    CHECK(retained.history[0].visitTimes.size() == kRecentHistoryVisits);
    CHECK(retained.history[0].visitTransitions.size() == retained.history[0].visitTimes.size());
    {
        // Summit's own visits keep the way they started: typed and link
        // visits count, a reload is recorded but does not, as in Firefox.
        Profile visits;
        const std::string page = "https://typed-visit.test/";
        CHECK(visits.VisitWithTransition({page, "Typed"}, 2));
        CHECK(visits.VisitWithTransition({page, "Reloaded"}, 9));
        CHECK(visits.VisitWithTransition({page, "Followed"}, 1));
        CHECK(visits.history.size() == 1 && HistoryVisitCount(visits.history[0]) == 2);
        std::vector<uint32_t> types;
        for (const auto& [time, type] : visits.history[0].visitTransitions) types.push_back(type);
        std::sort(types.begin(), types.end());
        CHECK((types == std::vector<uint32_t> {1, 2, 9}));
        CHECK(QueryBookmarks("place:transition=2", {}, visits.history).items.size() == 1);
        CHECK(QueryBookmarks("place:transition=3", {}, visits.history).items.empty());
        CHECK(!visits.VisitWithTransition({"about:blank", ""}, 2));
        // An unknown visit (0) records no type and counts.
        CHECK(visits.VisitWithTransition({page, ""}, 0) && HistoryVisitCount(visits.history[0]) == 3 && visits.history[0].visitTransitions.size() == 3);
    }
    std::cout << checks << " download visit checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
