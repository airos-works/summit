#include "core/BookmarkIO.h"
#include "core/BookmarkQueries.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static bool SQL(const std::string& library, const std::filesystem::path& path, const std::string& sql)
{
    void* handle = dlopen(library.c_str(), RTLD_NOW);
    if (!handle) return false;
    auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(handle, "sqlite3_open"));
    auto exec = reinterpret_cast<int (*)(void*, const char*, void*, void*, char**)>(dlsym(handle, "sqlite3_exec"));
    auto close = reinterpret_cast<int (*)(void*)>(dlsym(handle, "sqlite3_close"));
    void* database = nullptr;
    const bool ok = open && exec && close && open(path.c_str(), &database) == 0 && exec(database, sql.c_str(), nullptr, nullptr, nullptr) == 0;
    if (database) close(database);
    dlclose(handle);
    return ok;
}

int main()
{
    const std::string originalTZ = std::getenv("TZ") ? std::getenv("TZ") : "";
    setenv("TZ", "UTC", 1); tzset();
    constexpr int64_t now = 1700000000000000, second = 1000000, day = 86400 * second;
    Profile profile;
    PageRecord a{"https://time.test/a", "A", 0, false, 5};
    SetHistoryVisitTimes(a, {now, now - 3600 * second + 123, now - day, now - 2 * day, now - 3 * day});
    PageRecord b{"https://time.test/b", "B", 0, false, 2};
    SetHistoryVisitTimes(b, {now - 1800 * second, now - 3 * day});
    profile.history = {a, b, {"https://legacy.test/", "Unknown"}};
    auto& marks = profile.bookmarks;
    const auto folder = marks.AddFolder(kBookmarksBarFolder, "Nested");
    marks.Add({a.url, a.title}, folder);
    marks.Add({b.url, b.title}, kBookmarksBarFolder);
    marks.Add({"https://unvisited.test/", "Unvisited"}, folder);
    auto query = [&](std::string url, int64_t clock = 1700000000000000) { return QueryBookmarks(url, marks, profile.history, {}, clock); };
    auto range = [&](int64_t begin, int64_t end, const std::string& extra = "") {
        return query("place:beginTime=" + std::to_string(begin) + "&endTime=" + std::to_string(end) + extra);
    };
    auto one = [&](const BookmarkQueryResult& result, const std::string& url) {
        return result.error.empty() && result.items.size() == 1 && result.items.front().url == url;
    };
    CHECK(one(range(now - 3600 * second, now - 3599 * second), a.url)); // Not the latest visit.
    CHECK(one(range(now - 3600 * second + 123, now - 3600 * second + 123), a.url));
    CHECK(range(now - 3600 * second, now - 3600 * second + 122).items.empty());
    CHECK(range(now - 3600 * second + 124, now - 3599 * second).items.empty());
    CHECK(range(now + 1, now - 1).error.empty() && range(now + 1, now - 1).items.empty());
    CHECK(range(now - 3 * day, now).items.size() == 2);
    CHECK(range(0, 0).items.size() == 3); // Epoch zero means no bound, including unknown dates.
    CHECK(query("place:beginTimeRef=0&endTimeRef=0").items.size() == 3);
    CHECK(one(query("place:beginTimeRef=2&beginTime=-1000000"), a.url));
    CHECK(one(query("place:beginTimeRef=2"), a.url)); // Relative zero remains an active bound.
    CHECK(query("place:endTimeRef=2&endTime=-3600000001").items.size() == 2);
    CHECK(query("place:beginTimeRef=1").items.size() == 2);
    CHECK(query("place:endTimeRef=1").items.size() == 2);
    CHECK(query("place:beginTimeRef=1&endTimeRef=1&endTime=86399999999").items.size() == 2);
    CHECK(query("place:beginTimeRef=2&beginTime=-1000000", now + 2 * second).items.empty());
    CHECK(one(range(now - day, now - day, "&queryType=1"), a.url));
    CHECK(one(range(now - day, now - day, "&parent=toolbar_____"), a.url)); // Time makes the parent query recursive.
    CHECK(query("place:parent=toolbar_____").items.front().folder);
    CHECK(one(range(now - day, now - day, "&onlyBookmarked=1&minVisits=3&domain=time.test&terms=A"), a.url));
    CHECK(range(now - day, now - day, "&maxVisits=2").items.empty());
    CHECK(range(now - day, now - day, "&queryType=1&excludeItems=1").items.empty());
    CHECK(range(now - 3 * day, now - 3 * day, "&sort=4&maxResults=1").items.front().url == a.url);
    CHECK(range(now - 3 * day, now - 3 * day, "&sort=3&maxResults=1").items.front().url == b.url);
    for (const auto* invalid : {"place:beginTime=9223372036854775808", "place:endTime=-9223372036854775809",
            "place:beginTime=+1", "place:beginTimeRef=3", "place:endTimeRef=-1", "place:beginTime=1x",
            "place:endTime=", "place:beginTimeRef=2&beginTime=9223372036854775807"}) {
        auto result = query(invalid); CHECK(result.items.empty() && !result.error.empty());
    }
    CHECK(query("place:beginTime=-9223372036854775808&endTime=9223372036854775807").items.size() == 2);
    CHECK(!query("place:endTimeRef=2&endTime=-9223372036854775808", -1).error.empty());

    // Local midnight follows the offset at midnight, even on DST transition days.
    setenv("TZ", "America/New_York", 1); tzset();
    PageRecord dst{"https://dst.test/", "DST"};
    constexpr int64_t springMidnight = 1710046800000000;
    SetHistoryVisitTimes(dst, {springMidnight});
    profile.history.push_back(dst);
    CHECK(one(query("place:beginTimeRef=1&endTimeRef=1", 1710086400000000), dst.url));
    SetHistoryVisitTimes(profile.history.back(), {springMidnight - 1});
    CHECK(query("place:beginTimeRef=1&endTimeRef=1", 1710086400000000).items.empty());
    // Fixed offset zones with fractional hours must not be rounded to UTC days.
    setenv("TZ", "Asia/Kathmandu", 1); tzset();
    std::time_t utc = now / second; std::tm local {}; localtime_r(&utc, &local);
    CHECK(local.tm_hour == 3 && local.tm_min == 58); // now is 22:13:20 UTC.
    local.tm_hour = local.tm_min = local.tm_sec = 0; local.tm_isdst = -1;
    SetHistoryVisitTimes(profile.history.back(), {int64_t(std::mktime(&local)) * second});
    CHECK(one(query("place:beginTimeRef=1&endTimeRef=1"), dst.url));
    setenv("TZ", "UTC", 1); tzset(); profile.history.pop_back();

    // Recording and clearing individual visits preserves earlier visits today.
    Profile recorded;
    CHECK(recorded.Visit({a.url, "First"}, now / second - 7200));
    CHECK(recorded.Visit({a.url, "Second"}, now / second - 1800));
    CHECK(recorded.history.front().visitTimes.size() == 2);
    CHECK(recorded.ForgetVisitsSince(now / second - 3600) == 1);
    CHECK(recorded.history.front().visited == now / second - 7200 && recorded.history.front().visits == 1);
    CHECK(recorded.Visit({a.url, "Live"}));
    CHECK(recorded.history.front().visitTimes.front() <= HistoryNowMicroseconds());
    CHECK(HistoryNowMicroseconds() - recorded.history.front().visitTimes.front() < 5 * second);
    PageRecord legacy{a.url, "Legacy", now / second, false, 9, {now / second - 86400}};
    CHECK(HistoryVisitTimes(legacy) == std::vector<int64_t>({now, now - day}));
    legacy.visited = INT64_MAX;
    CHECK(HistoryVisitTimes(legacy) == std::vector<int64_t>({now - day}));
    PageRecord bounded{a.url, "Bounded"}; std::vector<int64_t> many;
    for (int i = 0; i < 200; ++i) many.push_back(now - i);
    for (int i = 1; i <= 90; ++i) many.push_back(now - i * day);
    many.push_back(-1); many.push_back(0); many.push_back(now);
    SetHistoryVisitTimes(bounded, many);
    CHECK(bounded.visitTimes.size() == kMaxHistoryVisitTimes && bounded.earlier.size() == kMaxEarlierVisits);
    CHECK(bounded.visitTimes[127] == now - 127 && bounded.visitTimes[128] == now - day);
    CHECK(bounded.visitTimes.back() == now - 60 * day);

    const auto directory = std::filesystem::temp_directory_path() / ("summit-time-queries-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    std::string error, library = "libsqlite3.so.0";
    if (const char* value = std::getenv("SUMMIT_TEST_SQLITE")) library = value;
    const auto firefox = directory / "places.sqlite", chrome = directory / "History";
    for (bool fx : {true, false}) {
        const int64_t offset = fx ? 0 : 11644473600000000;
        const std::string table = fx ? "moz_places" : "urls", visits = fx ? "moz_historyvisits" : "visits";
        const std::string last = fx ? "last_visit_date" : "last_visit_time", time = fx ? "visit_date" : "visit_time";
        const std::string fk = fx ? "place_id" : "url";
        const auto database = fx ? firefox : chrome;
        std::string sql = "CREATE TABLE " + table + "(id INTEGER PRIMARY KEY,url TEXT,title TEXT,visit_count INTEGER,hidden INTEGER," + last + " INTEGER);";
        sql += "CREATE TABLE " + visits + "(id INTEGER PRIMARY KEY," + fk + " INTEGER," + time + " INTEGER);";
        sql += "INSERT INTO " + table + " VALUES(1,'https://time.test/a','Imported',999,0," + std::to_string(now + offset + 321) + ");";
        for (size_t i = 0; i < many.size(); ++i) sql += "INSERT INTO " + visits + " VALUES(" + std::to_string(i + 1) + ",1," + std::to_string(many[i] + offset) + ");";
        CHECK(SQL(library, database, sql));
        std::vector<PageRecord> imported;
        CHECK(fx ? ReadFirefoxPlaces(database, library, nullptr, &imported, error) : ReadChromeHistory(database, library, imported, error));
        CHECK(error.empty() && imported.size() == 1);
        if (imported.empty()) continue;
        CHECK(imported[0].visitTimes.size() == kMaxHistoryVisitTimes);
        CHECK(imported[0].visitTimes.front() == now + 321 && imported[0].visitTimes.back() == now - 60 * day);
        CHECK(imported[0].earlier.size() == 60);
        // Importing visits for an existing URL still changes and persists history.
        Profile merged;
        merged.Visit({a.url, "Local title"}, (now - 1800 * second) / second);
        bool changed = false;
        CHECK(merged.ImportHistory(imported, &changed) == 0 && changed);
        CHECK(merged.history.front().title == "Local title" && merged.history.front().visits == 999);
        CHECK(merged.ImportHistory(imported, &changed) == 0 && !changed);
        CHECK(one(QueryBookmarks("place:beginTime=" + std::to_string(now - day) + "&endTime=" + std::to_string(now - day), marks, merged.history), a.url));
        CHECK(merged.Save(directory / "profile.json", error));
        auto loaded = Profile::Load(directory / "profile.json", error);
        CHECK(error.empty() && loaded.history.front().visitTimes == merged.history.front().visitTimes);
        CHECK(loaded.history.front().earlier == merged.history.front().earlier);
        CHECK(loaded.ForgetVisitsSince(now / second) == 2);
        CHECK(!QueryBookmarks("place:beginTime=" + std::to_string(now + 321) + "&endTime=" + std::to_string(now + 321), marks, loaded.history).items.size());
    }
    const auto smart = marks.Add({"place:beginTime=" + std::to_string(now - day) + "&endTime=" + std::to_string(now - day), "Earlier visit"}, kOtherBookmarksFolder);
    const auto rendered = RenderBookmarkQueryPage(marks, profile.history, smart, {}, "test");
    CHECK(rendered.find("href=\"https://time.test/a\"") != std::string::npos && rendered.find("href=\"https://time.test/b\"") == std::string::npos);
    CHECK(profile.Save(directory / "queries.json", error));
    auto restored = Profile::Load(directory / "queries.json", error);
    CHECK(error.empty() && restored.history[0].visitTimes == a.visitTimes);
    CHECK(one(QueryBookmarks(marks.Find(smart)->url, restored.bookmarks, restored.history), a.url));

    // The maximum retained timeline on every page remains within the profile limit.
    Profile large;
    for (int i = 0; i < 2000; ++i) { auto page = bounded; page.url += std::to_string(i); large.history.push_back(std::move(page)); }
    bool droppedChanged = true;
    CHECK(large.ImportHistory({{"https://expired.test/", "Expired", 1}}, &droppedChanged) == 0 && !droppedChanged);
    CHECK(large.Save(directory / "large.json", error));
    CHECK(std::filesystem::file_size(directory / "large.json") < 16 * 1024 * 1024);
    auto readLarge = Profile::Load(directory / "large.json", error);
    CHECK(error.empty() && readLarge.history.size() == 2000 && readLarge.history.back().visitTimes == bounded.visitTimes);
    std::filesystem::remove_all(directory);
    if (originalTZ.empty()) unsetenv("TZ"); else setenv("TZ", originalTZ.c_str(), 1);
    tzset();
    std::cout << checks << " bookmark time query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
