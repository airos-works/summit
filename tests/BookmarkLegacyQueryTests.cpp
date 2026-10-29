#include "core/BookmarkQueries.h"
#include "core/Profile.h"
#include <cstdlib>
#include <ctime>
#include <iostream>

// Annotation clauses and sorts, keyword sorts, folder-shortcut and grouped
// filter semantics from Firefox's Places query builder (Firefox 3.6, 52, 60
// and 158 sources; see docs/bookmarks.md, "Annotations and legacy options").
using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static std::string URLs(const BookmarkQueryResult& result)
{
    std::string text;
    for (const auto& node : result.items) text += (text.empty() ? "" : ",") + node.url;
    return text;
}
static std::string Titles(const std::vector<BookmarkNode>& nodes)
{
    std::string text;
    for (const auto& node : nodes) text += (text.empty() ? "" : ",") + node.title;
    return text;
}
static std::string Sites(const BookmarkQueryResult& result)
{
    std::string text;
    for (const auto& group : result.items) {
        text += (text.empty() ? "" : ";") + group.title + ":";
        for (size_t i = 0; i < group.children.size(); ++i) text += (i ? "," : "") + group.children[i].url;
    }
    return text;
}

int main()
{
    setenv("TZ", "UTC", 1); tzset();
    constexpr int64_t hour = 3600000000LL, now = 1800000000000000LL;
    Profile profile;
    auto& marks = profile.bookmarks;
    const auto feeds = marks.AddFolder(kBookmarksBarFolder, "Feeds");
    const auto feed = marks.Add({"https://feed.test/item", "Feed item"}, feeds);
    const auto a = marks.Add({"https://alpha.test/a", "Alpha"}, kBookmarksBarFolder);
    const auto b = marks.Add({"https://beta.test/b", "Beta"}, kBookmarksBarFolder);
    const auto c = marks.Add({"https://gamma.test/c", "Gamma"}, kBookmarksBarFolder);
    CHECK(feeds && feed && a && b && c);
    BookmarkMetadata metadata;
    metadata.description = "livemark folder"; CHECK(marks.SetMetadata(feeds, metadata));
    metadata.description = "zeta notes"; metadata.keywords = {{"al", "", "windows-1252"}}; CHECK(marks.SetMetadata(a, metadata));
    metadata.description = "alpha notes"; metadata.keywords = {{"be", "", ""}}; CHECK(marks.SetMetadata(b, metadata));
    metadata = {}; metadata.tags = {"Work"}; CHECK(marks.SetMetadata(c, metadata));
    auto page = [&](const std::string& url, const std::string& title, int count, std::vector<int64_t> times, uint32_t transition = 1) {
        PageRecord record{url, title, 0, false, count};
        record.visitCountKnown = true;
        for (auto time : times) record.visitTransitions.emplace_back(time, transition);
        SetHistoryVisitTimes(record, times);
        profile.history.push_back(std::move(record));
    };
    page("https://alpha.test/a", "Alpha", 3, {now - 2 * hour, now - 3 * hour, now - 4 * hour});
    page("https://beta.test/b", "Beta", 1, {now - 100 * hour});
    page("https://gamma.test/c", "Gamma", 1, {now - 2 * hour});
    page("https://gamma.test/other", "Gamma other", 1, {now - 5 * hour});
    page("https://delta.test/d1", "Delta one", 2, {now - hour, now - 200 * hour});
    page("https://delta.test/d2", "Delta download", 0, {now - hour}, 7);
    page("https://zero.test/z", "Zero download", 0, {now - hour}, 7);
    page("https://feed.test/item", "Feed item", 1, {now - 300 * hour});
    auto query = [&](const std::string& url) { return QueryBookmarks(url, marks, profile.history, {}, now); };
    const auto all = query("place:");
    CHECK(all.error.empty() && all.items.size() == 8);

    // annotation= / !annotation= test Firefox 60's page annotations (moz_annos).
    // Summit retains URIProperties/characterSet with keywords; nothing else.
    CHECK(URLs(query("place:annotation=URIProperties%2FcharacterSet")) == "https://alpha.test/a");
    CHECK(URLs(query("place:queryType=1&annotation=URIProperties%2FcharacterSet")) == "https://alpha.test/a");
    CHECK(query("place:!annotation=URIProperties%2FcharacterSet").items.size() == 7);
    CHECK(query("place:queryType=1&!annotation=URIProperties%2FcharacterSet").items.size() == 3);
    // Descriptions were item annotations; annotation= never matched them.
    CHECK(query("place:queryType=1&annotation=bookmarkProperties%2Fdescription").error.empty());
    CHECK(query("place:queryType=1&annotation=bookmarkProperties%2Fdescription").items.empty());
    CHECK(query("place:annotation=livemark%2FfeedURI").items.empty() && query("place:annotation=livemark%2FfeedURI").error.empty());
    CHECK(query("place:!annotation=livemark%2FfeedURI").items.size() == 8);
    CHECK(query("place:annotation=urIProperties%2FcharacterSet").items.empty()); // Names compare exactly.
    CHECK(query("place:annotation=").items.size() == 8); // An empty name is no condition.
    CHECK(query("place:annotation=URIProperties%2FcharacterSet&!annotation=URIProperties%2FcharacterSet").items.size() == 7); // Last wins.
    CHECK(URLs(query("place:annotation=x&OR&uri=https%3A%2F%2Fbeta.test%2Fb")) == "https://beta.test/b");
    CHECK(query("place:!annotation=x&OR&uri=https%3A%2F%2Fbeta.test%2Fb").items.size() == 8);
    CHECK(URLs(query("place:annotation=URIProperties%2FcharacterSet&domain=beta.test")).empty());
    CHECK(!query("place:annotation=%zz").error.empty() && query("place:annotation=%zz").items.empty());

    // sort=19/20 with sortingAnnotation (Firefox 60): bookmark rows use item
    // annotations, history rows page annotations; missing values sort as
    // empty, two missing values fall back to title order.
    const std::string description = "&sortingAnnotation=bookmarkProperties%2Fdescription";
    CHECK(URLs(query("place:queryType=1&sort=19" + description))
        == "https://feed.test/item,https://gamma.test/c,https://beta.test/b,https://alpha.test/a");
    CHECK(URLs(query("place:queryType=1&sort=20" + description))
        == "https://alpha.test/a,https://beta.test/b,https://gamma.test/c,https://feed.test/item");
    CHECK(URLs(query("place:queryType=1&sort=19")) == URLs(query("place:queryType=1"))); // No name: no order.
    auto charset = query("place:sort=20&sortingAnnotation=URIProperties%2FcharacterSet");
    CHECK(charset.items.size() == 8 && charset.items.front().url == "https://alpha.test/a");
    charset = query("place:sort=19&sortingAnnotation=URIProperties%2FcharacterSet");
    CHECK(charset.items.back().url == "https://alpha.test/a" && charset.items.front().title == "Beta");
    // No SQL ORDER BY: the limit takes stored rows, then they are sorted.
    CHECK(URLs(query("place:queryType=1&sort=20&maxResults=2" + description)) == "https://alpha.test/a,https://feed.test/item");

    // sort=9/10: keywords of bookmark rows (Firefox 52 and earlier), title fallback.
    CHECK(URLs(query("place:queryType=1&sort=9")) == "https://feed.test/item,https://gamma.test/c,https://alpha.test/a,https://beta.test/b");
    CHECK(URLs(query("place:queryType=1&sort=10")) == "https://beta.test/b,https://alpha.test/a,https://gamma.test/c,https://feed.test/item");
    CHECK(URLs(query("place:sort=9")) == URLs(query("place:sort=1"))); // History rows have no keyword.
    CHECK(!query("place:sort=15").error.empty() && !query("place:sort=16").error.empty());
    CHECK(query("place:sort=21").error.empty() && query("place:sort=22").items.size() == 8);

    // Virtual roots sort descendants by item annotation and keyword too.
    auto roots = query("place:type=8&sort=19" + description);
    CHECK(roots.error.empty() && roots.items.size() == 2 && Titles(roots.items[0].children) == "Gamma,Beta,Feeds,Alpha");
    roots = query("place:type=8&sort=9");
    CHECK(roots.error.empty() && Titles(roots.items[0].children) == "Feeds,Gamma,Alpha,Beta");

    // Firefox 3's excludeItemIfParentHasAnnotation (item annotations of the
    // result's folder) and Firefox 60's excludeReadOnlyFolders (no Summit
    // folder carries placesInternal/READ_ONLY).
    CHECK(query("place:queryType=1&excludeItemIfParentHasAnnotation=bookmarkProperties%2Fdescription").items.size() == 3);
    CHECK(query("place:queryType=1&excludeItemIfParentHasAnnotation=livemark%2FfeedURI").items.size() == 4);
    CHECK(query("place:excludeItemIfParentHasAnnotation=bookmarkProperties%2Fdescription").items.size() == 8); // History rows.
    CHECK(query("place:queryType=1&excludeReadOnlyFolders=1").items.size() == 4);
    CHECK(!query("place:excludeReadOnlyFolders=maybe").error.empty());

    // Tags compare case-insensitively (lower(tag title) IN lower(tags)).
    CHECK(URLs(query("place:queryType=1&tag=work")) == "https://gamma.test/c");
    CHECK(query("place:queryType=1&tag=WORK&!tags=1").items.size() == 3);

    // Folder shortcuts ignore visit counts, transitions and annotations.
    const auto bar = query("place:parent=toolbar_____");
    CHECK(Titles(bar.items) == "Feeds,Alpha,Beta,Gamma");
    CHECK(Titles(query("place:parent=toolbar_____&minVisits=50&transition=7&annotation=x&!tags=1").items) == Titles(bar.items));
    CHECK(Titles(query("place:type=1&parent=toolbar_____").items) == Titles(bar.items));
    CHECK(query("place:parent=toolbar_____&annotation=x&maxResults=10").items.empty()); // A limit searches descendants.

    // Site grouping: hosts of matching pages with counted visits; each host's
    // child query has only the host and the first group's time range.
    const auto range = "&beginTime=" + std::to_string(now - 10 * hour) + "&endTime=" + std::to_string(now);
    auto sites = query("place:type=4&sort=5");
    CHECK(sites.error.empty() && Titles(sites.items) == "alpha.test,beta.test,delta.test,feed.test,gamma.test"); // zero.test has no counted visit.
    CHECK(sites.items[2].children.size() == 2); // Its download-only page is still a child.
    CHECK(Sites(query("place:type=4&sort=5" + range + "&tag=work")) == "gamma.test:https://gamma.test/c,https://gamma.test/other");
    CHECK(Sites(query("place:type=4&sort=5" + range + "&minVisits=2")) == "alpha.test:https://alpha.test/a;delta.test:https://delta.test/d1,https://delta.test/d2");
    CHECK(Sites(query("place:type=4&sort=5" + range + "&annotation=URIProperties%2FcharacterSet")) == "alpha.test:https://alpha.test/a");
    CHECK(query("place:type=4" + range + "&transition=7").items.empty()); // Download pages have no counted visit.
    CHECK(Sites(query("place:type=4&sort=5" + range + "&uri=https%3A%2F%2Fdelta.test%2Fd1")) == "delta.test:https://delta.test/d1,https://delta.test/d2");
    // A later group can select a host with no page in the first group's range.
    CHECK(Sites(query("place:type=4&sort=5" + range + "&tag=work&OR&uri=https%3A%2F%2Fbeta.test%2Fb"))
        == "beta.test:;gamma.test:https://gamma.test/c,https://gamma.test/other");
    CHECK(Titles(query("place:type=4&excludeItems=1" + range + "&minVisits=2").items) == "alpha.test,delta.test");
    for (const auto& bad : std::vector<std::string>{"place:type=4&minVisits=2", "place:type=4&tag=work&OR" + range,
            "place:type=4&beginTime=1&tag=work", "place:type=4" + range + "&terms=Gamma", "place:type=4" + range + "&parent=toolbar_____",
            "place:type=3&annotation=x", "place:type=5" + range, "place:type=1&parent=toolbar_____&terms=a"}) {
        const auto result = query(bad);
        CHECK(!result.error.empty() && result.items.empty() && result.error.find("Firefox cannot") != std::string::npos);
    }
    std::cout << checks << " legacy query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
