#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <unistd.h>
using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static std::string Titles(const BookmarkQueryResult& result)
{
    std::string text;
    for (const auto& node : result.items) text += (text.empty() ? "" : ",") + node.title;
    return text;
}
static const BookmarkNode* FindOrigin(const BookmarkNode& node, const BookmarkOrigin& origin)
{
    if (node.origin == origin) return &node;
    for (const auto& child : node.children)
        if (auto* found = FindOrigin(child,origin)) return found;
    return nullptr;
}
int main()
{
    Profile profile;
    auto& marks = profile.bookmarks;
    const std::string a = "https://alpha.test/a", b = "https://sub.alpha.test/b", c = "https://beta.test/c", d = "https://outside.test/d";
    const auto aid = marks.Add({a, "Alpha red"}, kBookmarksBarFolder, kBookmarkEnd, 10);
    const auto nested = marks.AddFolder(kBookmarksBarFolder, "Nested");
    const auto bid = marks.Add({b, "Beta blue"}, nested, kBookmarkEnd, 20);
    const auto cid = marks.Add({c, "Gamma red"}, kOtherBookmarksFolder, kBookmarkEnd, 30);
    marks.AddSeparator(nested);
    marks.SetMetadata(aid, {"", {"work", "red"}});
    marks.SetMetadata(bid, {"", {"work", "blue"}});
    marks.SetMetadata(cid, {"", {"read", "red"}});
    profile.history = {{a,"Alpha red",7,false,8},{b,"Beta blue",2,false,3},{c,"Gamma red",3,false,5},{d,"Delta green",4,false,1}};
    profile.history[0].visitTimes = {7000007,1000001};
    profile.history[1].visitTimes = {2000002};
    profile.history[2].visitTimes = {3000003};
    profile.history[3].visitTimes = {4000004};
    auto query = [&](std::string_view url) { return QueryBookmarks(url,marks,profile.history,{},10000000); };
    CHECK(Titles(query("place:terms=Alpha%20red&OR&terms=Gamma&sort=8")) == "Alpha red,Gamma red");
    CHECK(Titles(query("place:terms=Alpha%20red&maxVisits=5&OR&domain=beta.test")) == "Gamma red");
    CHECK(Titles(query("place:terms=Alpha%20red&OR&minVisits=4&sort=8")) == "Alpha red,Gamma red");
    CHECK(Titles(query("place:terms=Gamma&OR&terms=Alpha%20red&sort=8&maxResults=1")) == "Alpha red");
    CHECK(Titles(query("place:maxResults=1&terms=Gamma&sort=8&OR&terms=Alpha%20red")) == "Alpha red");
    CHECK(Titles(query("place:sort=7&terms=Gamma&OR&terms=Alpha%20red&sort=8&maxResults=1")) == "Alpha red");
    CHECK(Titles(query("place:terms=Alpha%20red&OR&terms=Gamma&OR&terms=Delta&sort=1&maxResults=2")) == "Alpha red,Delta green");
    CHECK(Titles(query("place:terms=Alpha%20red&OR&terms=Gamma&OR&terms=Delta&sort=2&maxResults=2")) == "Delta green,Alpha red");
    CHECK(query("place:terms=Alpha%20red&OR&terms=Alpha%20red").items.size() == 1);
    CHECK(Titles(query("place:domain=alpha.test&domainIsHost=1&terms=Alpha%20red&OR&domain=alpha.test&terms=Beta")) == "Alpha red,Beta blue");
    CHECK(Titles(query("place:onlyBookmarked=1&terms=Delta&OR&uri=https%3A%2F%2Foutside.test%2Fd")) == "Delta green");
    CHECK(Titles(query("place:queryType=1&tag=work&!tags=1&OR&tag=blue&sort=1")) == "Beta blue,Gamma red");
    CHECK(Titles(query("place:queryType=1&tag=work&tag=red&OR&tag=read&sort=1")) == "Alpha red,Gamma red");
    CHECK(query("place:queryType=1&tag=work&OR&tag=red").items.size() == 3);
    CHECK(Titles(query("place:beginTime=1000001&endTime=1000001&OR&beginTime=2000002&endTime=2000002")) == "Alpha red,Beta blue");
    CHECK(Titles(query("place:beginTimeRef=2&beginTime=-2999993&endTimeRef=2&endTime=-2999993&OR&beginTime=2000002&endTime=2000002")) == "Alpha red,Beta blue");
    CHECK(Titles(query("place:beginTime=6000000&endTime=5000000&OR&terms=Gamma")) == "Gamma red");
    CHECK(Titles(query("place:terms=Alpha%20red&OR&terms=Delta&queryType=1")) == "Alpha red");
    CHECK(Titles(query("place:queryType=1&terms=Alpha%20red&OR&terms=Delta&queryType=0")) == "Alpha red,Delta green");
    CHECK(query("place:queryType=1&terms=Alpha%20red&OR&terms=Gamma&excludeItems=1").items.empty());

    // More than one branch makes even a single folder condition a descendant
    // search; it must not expose folders, separators or saved queries as items.
    CHECK(Titles(query("place:parent=toolbar_____&OR&parent=unfiled_____&sort=1")) == "Alpha red,Beta blue,Gamma red");
    CHECK(Titles(query("place:parent=toolbar_____&terms=Beta&OR&parent=unfiled_____&terms=red&sort=1")) == "Beta blue,Gamma red");
    CHECK(Titles(query("place:folder=TOOLBAR&OR&folder=UNFILED_BOOKMARKS&sort=1")) == "Alpha red,Beta blue,Gamma red");
    CHECK(Titles(query("place:parent=toolbar_____&OR&terms=Gamma&queryType=0&sort=1")) == "Alpha red,Beta blue,Gamma red");
    CHECK(Titles(query("place:parent=toolbar_____&OR")) == "Alpha red,Beta blue");
    CHECK(Titles(query("place:OR&terms=Gamma")) == "Gamma red");
    CHECK(Titles(query("place:terms=Gamma&OR&OR")) == "Gamma red");
    CHECK(query("place:OR&OR").items.size() == 4);
    CHECK(query("place:domainIsHost=1&OR&terms=Gamma").items.size() == 1);
    CHECK(query("place:queryType=1&onlyBookmarked=1&OR&terms=Gamma").items.size() == 1);
    for (const auto* invalid : {"place:terms=Alpha%20red&OR&transition=invalid", "place:terms=Alpha%20red&OR&annotation=foo",
            "place:terms=Alpha%20red&OR&beginTimeRef=2&beginTime=9223372036854775807",
            "place:terms=Alpha%20red&OR&parent=missing_____", "place:terms=Alpha%20red&OR&terms=%ff%00",
            "place:terms=Alpha%20red&or&terms=Gamma", "place:terms=Alpha%20red&%4f%52&terms=Gamma"}) {
        const auto result = query(invalid);
        CHECK(!result.error.empty() && result.items.empty());
    }
    std::string boundary = "place:terms=Alpha%20red";
    for (int i = 0; i < 255; ++i) boundary += "&OR";
    CHECK(query(boundary).error.empty() && query(boundary).items.size() == 1);
    CHECK(!query(boundary + "&OR").error.empty());
    CHECK(query("place:terms=Alpha%26OR%26Gamma").items.empty());
    CHECK(query("place:terms=OR").items.size() == 2); // Literal text matches the work tag.

    const auto shortcut = marks.Add({"place:parent=toolbar_____&OR&parent=unfiled_____", "Combined folders"}, kBookmarksBarFolder);
    const auto queryID = marks.Add({"place:terms=Alpha%20red&OR&terms=Gamma&sort=8", "Combined history"}, kOtherBookmarksFolder);
    CHECK(query("place:queryType=1&terms=Combined&OR&terms=Alpha%20red").items.size() == 1);
    auto hierarchy = query("place:parent=toolbar_____&excludeQueries=1");
    CHECK(std::none_of(hierarchy.items.begin(),hierarchy.items.end(),[&](const auto& node) { return node.id == shortcut; }));
    auto groups = query("place:type=6&tag=work&OR&tag=red&sort=1&maxResults=2");
    CHECK(groups.error.empty() && groups.items.size() == 2 && groups.items[0].title == "blue" && groups.items[1].title == "read");
    CHECK(groups.items[0].children.size() == 1 && groups.items[1].children.size() == 1);
    const auto duplicate = marks.Add({a,"Alpha copy"},kOtherBookmarksFolder);
    auto distinct = query("place:queryType=1&terms=Alpha%20red&OR&domain=alpha.test&sort=1");
    CHECK(distinct.items.size() == 3 && distinct.items[0].id == duplicate && distinct.items[1].id == aid && distinct.items[2].id == bid);
    CHECK(marks.Remove(duplicate));
    CHECK(marks.Edit(cid,"Renamed red",c));
    CHECK(Titles(query("place:queryType=1&terms=Alpha%20red&OR&terms=Gamma")) == "Alpha red");
    CHECK(marks.Move(bid,kOtherBookmarksFolder,kBookmarkEnd));
    CHECK(Titles(query("place:parent=toolbar_____&OR&terms=Renamed&sort=1")) == "Alpha red,Renamed red");

    // A combined query's folder references retain the originating profile.
    Bookmarks scoped;
    const std::string combined = "place:folder=10&OR&parent=second______&sort=1";
    for (const auto* scope : {"first-profile", "second-profile"}) {
        BookmarkNode source; source.folder = true; source.origin = {scope,"root________",1};
        source.children = {{0,"One","",true,0,{}},{0,"Two","",true,0,{}},{0,"Combined",combined,false,0,{}}};
        source.children[0].origin = {scope,"first_______",10};
        source.children[1].origin = {scope,"second______",20};
        source.children[2].origin = {scope,"combined____",30};
        source.children[0].children = {{0,std::string(scope)+" A","https://"+std::string(scope)+".test/a",false,0,{}}};
        source.children[1].children = {{0,std::string(scope)+" B","https://"+std::string(scope)+".test/b",false,0,{}}};
        CHECK(scoped.Import(source,scope).added == 3);
        auto* saved = FindOrigin(scoped.Other(),{scope,"combined____",30});
        CHECK(saved && Titles(QueryBookmarks(saved->url,scoped,{},saved->origin)) == std::string(scope)+" A,"+scope+" B");
    }
    const auto directory = std::filesystem::temp_directory_path() / ("summit-combined-queries-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    std::string error;
    CHECK(profile.Save(directory/"profile.json",error));
    auto loaded = Profile::Load(directory/"profile.json",error);
    CHECK(error.empty() && loaded.bookmarks.FindURL(marks.Find(queryID)->url));
    CHECK(Titles(QueryBookmarks(marks.Find(queryID)->url,loaded.bookmarks,loaded.history)) == "Alpha red,Gamma red");
    BookmarkNode html;
    CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(scoped),html,error));
    Bookmarks roundtrip;
    CHECK(roundtrip.Import(html,"HTML").added == 6);
    auto* saved = FindOrigin(roundtrip.Other(),{"second-profile","combined____",30});
    CHECK(saved && Titles(QueryBookmarks(saved->url,roundtrip,{},saved->origin)) == "second-profile A,second-profile B");
    CHECK(!roundtrip.Import(html,"HTML").Changed());
    const auto rendered = RenderBookmarkQueryPage(marks,profile.history,queryID,{},"test");
    CHECK(rendered.find("Alpha red") < rendered.find("Gamma red") && rendered.find("OR") != std::string::npos);
    std::filesystem::remove_all(directory);
    std::cout << checks << " combined bookmark query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
