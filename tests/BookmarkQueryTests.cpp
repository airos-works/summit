#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static std::string URLs(const BookmarkQueryResult& result)
{
    std::string text;
    for (const auto& node : result.items) text += (text.empty() ? "" : ",") + node.url;
    return text;
}

int main()
{
    Profile profile;
    auto& marks = profile.bookmarks;
    const auto a = marks.Add({"https://example.com/a", "Zebra A"}, kBookmarksBarFolder, kBookmarkEnd, 10);
    const auto b = marks.Add({"https://sub.example.com/b", "Alpha B"}, kOtherBookmarksFolder, kBookmarkEnd, 30);
    const auto c = marks.Add({"https://other.test/c", "Beta C++"}, kOtherBookmarksFolder, kBookmarkEnd, 20);
    marks.Add({"file:///tmp/example", "Local"}, kOtherBookmarksFolder, kBookmarkEnd, 40);
    marks.SetMetadata(a, {"", {"work", "C++"}});
    marks.SetMetadata(b, {"", {"work", "read"}});
    marks.SetMetadata(c, {"", {"read"}});
    profile.history = {{"https://example.com/a", "History A", 100, false, 9},
        {"https://sub.example.com/b", "History B", 300, false, 2},
        {"https://other.test/c", "History C", 200, false, 4},
        {"https://unbookmarked.test/", "Unbookmarked", 400, false, 0}};
    auto query = [&](std::string_view url) { return QueryBookmarks(url, marks, profile.history); };
    CHECK(IsBookmarkQueryURL("place:") && IsBookmarkQueryURL("PLACE:sort=8"));
    CHECK(!IsBookmarkQueryURL(" place:sort=8") && !IsBookmarkQueryURL("https://place:"));
    CHECK(!IsBookmarkQueryURL(std::string("place:sort=8\0x", 14)));
    CHECK(!IsBookmarkQueryURL("place:" + std::string(65536, 'a')));
    CHECK(IsBookmarkableURL("place:parent=unknown") && NormalizeBookmarkAddress(" place:sort=8 ") == "place:sort=8");
    CHECK(query("https://example.com").items.empty() && !query("https://example.com").error.empty());
    CHECK(URLs(query("place:sort=8&maxResults=2")) == "https://example.com/a,https://other.test/c");
    CHECK(URLs(query("place:sort=7&maxResults=1")) == "https://unbookmarked.test/");
    CHECK(URLs(query("place:sort=4&maxResults=1")) == "https://unbookmarked.test/");
    CHECK(URLs(query("place:sort=3&maxResults=1")) == "https://example.com/a");
    CHECK(query("place:queryType=1").items.size() == 4);
    CHECK(URLs(query("place:queryType=1&sort=12&maxResults=2")) == "file:///tmp/example,https://sub.example.com/b");
    CHECK(URLs(query("place:queryType=1&sort=11&maxResults=1")) == "https://example.com/a");
    CHECK(URLs(query("place:queryType=1&sort=1&maxResults=2")) == "https://sub.example.com/b,https://other.test/c");
    CHECK(URLs(query("place:queryType=1&sort=2&maxResults=2")) == "https://other.test/c,https://sub.example.com/b");
    CHECK(URLs(query("place:queryType=1&sort=5&maxResults=1")) == "file:///tmp/example");
    CHECK(URLs(query("place:queryType=1&sort=6&maxResults=1")) == "https://sub.example.com/b");
    CHECK(query("place:queryType=1&sort=17").error.empty() && query("place:queryType=1&sort=18").error.empty());
    CHECK(query("place:sort=8&maxResults=0").items.size() == 4);
    CHECK(query("place:sort=8&maxResults=4294967295").items.size() == 4);
    CHECK(query("place:minVisits=3&maxVisits=9").items.size() == 2);
    CHECK(query("place:minVisits=10").items.empty());
    CHECK(query("place:queryType=1&maxVisits=0").items.size() == 1);
    CHECK(query("place:onlyBookmarked=1").items.size() == 3);
    CHECK(query("place:onlyBookmarked=0").items.size() == 4);
    CHECK(query("place:domain=example.com").items.size() == 2);
    CHECK(URLs(query("place:domain=example.com&domainIsHost=true")) == "https://example.com/a");
    CHECK(query("place:domain=ample.com").items.empty());
    CHECK(query("place:domain=EXAMPLE.COM").items.size() == 2);
    CHECK(URLs(query("place:queryType=1&domain=")) == "file:///tmp/example");
    CHECK(URLs(query("place:uri=https%3A%2F%2Fother.test%2Fc")) == "https://other.test/c");
    CHECK(query("place:uri=https%3A%2F%2Fother.test%2F").items.empty());
    CHECK(URLs(query("place:queryType=1&terms=ALPHA%20example")) == "https://sub.example.com/b");
    CHECK(URLs(query("place:queryType=1&terms=C++")) == "https://example.com/a,https://other.test/c");
    CHECK(URLs(query("place:queryType=1&tag=C%2B%2B")) == "https://example.com/a");
    CHECK(query("place:queryType=1&tag=work").items.size() == 2);
    CHECK(URLs(query("place:queryType=1&tag=work&tag=read")) == "https://sub.example.com/b");
    CHECK(query("place:queryType=1&tag=work&tag=work").items.size() == 2);
    CHECK(URLs(query("place:queryType=1&tag=work&tag=read&!tags=1")) == "file:///tmp/example");
    CHECK(query("place:tag=read").items.size() == 2);
    CHECK(query("place:queryType=1&excludeItems=1").items.empty());
    auto tags = query("place:type=6&sort=1&excludeItems=1");
    CHECK(tags.error.empty() && tags.items.size() == 3 && tags.items[0].folder && tags.items[0].title == "C++");
    CHECK(tags.items.back().title == "work" && tags.items.back().children.size() == 2);
    CHECK(query("place:type=6&sort=2&maxResults=1").items.front().title == "work");
    CHECK(!query("place:type=6&sort=12").error.empty());
    for (const auto* invalid : {"place:sort=8junk", "place:maxResults=-1", "place:maxResults=4294967296",
        "place:queryType=3", "place:sort=99", "place:minVisits=-2", "place:excludeItems=maybe", "place:terms=%00",
        "place:terms=%zz", "place:terms=%", "place:terms=%2", "place:sort=999999999999999999999999999999"}) {
        const auto result = query(invalid); CHECK(result.items.empty() && !result.error.empty());
    }
    for (const auto* unsupported : {"place:parent=unknown", "place:folder=3", "place:transition=2",
        "place:OR", "place:annotation=test", "place:includeHidden=1", "place:originalTitle=1", "place:type=3", "place:sort=14", "place:sort=22"}) {
        const auto result = query(unsupported); CHECK(result.items.empty() && !result.error.empty());
    }
    CHECK(query("place:includeHidden=false&originalTitle=0&asyncEnabled=1&expandQueries=1").items.size() == 4);
    std::string many = "place:";
    for (size_t i = 0; i < 257; ++i) many += "sort=8&";
    CHECK(!query(many).error.empty());

    const auto smart = marks.Add({"place:sort=8&maxResults=2", "Most visited <safe>"}, kBookmarksBarFolder);
    const auto unsupported = marks.Add({"place:parent=unknown", "Folder shortcut"}, kOtherBookmarksFolder);
    marks.SetMetadata(smart, {"A live search", {"smart"}, {{"popular", "", ""}}});
    CHECK(marks.Find(smart) && !marks.Find(smart)->folder && marks.Find(smart)->children.empty());
    CHECK(query("place:queryType=1&excludeQueries=1").items.size() == 5); // Folder shortcuts remain included.
    CHECK(query("place:queryType=1&excludeItems=1").items.size() == 2);
    CHECK(marks.ResolveKeyword("popular")->url == "place:sort=8&maxResults=2");
    auto snapshot = query(marks.Find(smart)->url);
    profile.history[1].visits = 20;
    CHECK(URLs(snapshot) == "https://example.com/a,https://other.test/c");
    CHECK(URLs(query(marks.Find(smart)->url)) == "https://sub.example.com/b,https://example.com/a");
    const auto d = marks.Add({"https://new.test/", "New"}, kOtherBookmarksFolder, kBookmarkEnd, 50);
    CHECK(URLs(query("place:queryType=1&sort=12&maxResults=1&excludeQueries=1")) == "https://new.test/");
    CHECK(marks.Remove(d));
    CHECK(query("place:queryType=1&excludeQueries=1").items.size() == 5);
    const auto manager = RenderBookmarksPage(marks, {}, "ab12");
    CHECK(manager.find("href=\"place:") == std::string::npos && manager.find("summit-bookmarks:query?token=ab12&amp;id=") != std::string::npos);
    CHECK(manager.find("Smart folder · place:") != std::string::npos);
    const auto page = RenderBookmarkQueryPage(marks, profile.history, smart, {}, "ab12");
    CHECK(page.find("Most visited &lt;safe&gt;") != std::string::npos && page.find("Most visited <safe>") == std::string::npos);
    CHECK(page.find("href=\"https://sub.example.com/b\"") < page.find("href=\"https://example.com/a\""));
    CHECK(page.find("Edit Smart Folder") != std::string::npos && page.find("All Bookmarks") != std::string::npos);
    CHECK(RenderBookmarkQueryPage(marks, profile.history, unsupported, {}, "ab12").find("target is missing or was not imported") != std::string::npos);
    CHECK(RenderBookmarkQueryPage(marks, profile.history, 9999, {}, "ab12").find("removed or changed") != std::string::npos);
    const auto script = marks.Add({"javascript:alert(1)", "Script"}, kOtherBookmarksFolder);
    const auto all = marks.Add({"place:queryType=1&excludeQueries=1", "All"}, kBookmarksBarFolder);
    const auto inert = RenderBookmarkQueryPage(marks, profile.history, all, {}, "ab12");
    CHECK(inert.find("href=\"javascript:") == std::string::npos && inert.find("Bookmarklet · Run from") != std::string::npos);
    CHECK(marks.Remove(script));
    std::string error;
    const auto path = std::filesystem::temp_directory_path() / ("summit-query-test-" + std::to_string(getpid()) + ".json");
    CHECK(profile.Save(path, error));
    auto loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.bookmarks.FindURL("place:parent=unknown"));
    CHECK(loaded.bookmarks.FindURL("place:sort=8&maxResults=2")->metadata.description == "A live search");
    CHECK(URLs(QueryBookmarks("place:sort=8&maxResults=2", loaded.bookmarks, loaded.history)) == "https://sub.example.com/b,https://example.com/a");
    BookmarkNode imported;
    CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(marks), imported, error));
    Bookmarks roundtrip;
    auto first = roundtrip.Import(imported, "Imported");
    CHECK(first.added == marks.Count() && first.unsupported == 0);
    CHECK(roundtrip.FindURL("place:parent=unknown") && roundtrip.FindURL("place:sort=8&maxResults=2"));
    CHECK(!roundtrip.Import(imported, "Imported").Changed());
    CHECK(roundtrip.Edit(roundtrip.FindURL("place:sort=8&maxResults=2")->id, "Changed", "place:sort=4"));
    CHECK(roundtrip.FindURL("place:sort=4") && !roundtrip.FindURL("place:sort=8&maxResults=2"));
    profile.history.clear();
    CHECK(query("place:sort=8").items.empty());
    CHECK(!query("place:queryType=1&excludeQueries=1").items.empty());
    std::filesystem::remove(path);
    std::cout << checks << " bookmark query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
