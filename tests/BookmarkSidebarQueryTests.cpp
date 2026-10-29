#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace summit;
using nlohmann::json;
static int checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr<<__LINE__<<": " #x "\n"; } } while (0)
static const BookmarkNode* Find(const BookmarkNode& node,const std::string& scope,const std::string& guid)
{
    if (node.origin.scope==scope && node.origin.guid==guid) return &node;
    for (const auto& child:node.children) if (const auto* found=Find(child,scope,guid)) return found;
    return nullptr;
}
static const BookmarkNode* Find(const Bookmarks& bookmarks,const std::string& scope,const std::string& guid)
{
    if (const auto* found=Find(bookmarks.Bar(),scope,guid)) return found;
    return Find(bookmarks.Other(),scope,guid);
}
int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    setenv("TZ","America/New_York",1);tzset();
    json fixture;std::ifstream(argv[1])>>fixture;
    Profile profile;
    auto build=[&](const std::string& scope,bool second=false) {
        BookmarkNode root{0,"Imported library","",true,0,{}};root.origin={scope,"root________",1};
        for (const auto& seed:fixture["seed"]) {
            BookmarkNode folder{0,seed["title"],"",true,0,{}};folder.origin={scope,seed["parentGuid"],int64_t(root.children.size()+2)};
            const auto url=second?"https://second.library.example/"+seed["title"].get<std::string>():seed["url"].get<std::string>();
            BookmarkNode page{0,seed["title"],url,false,0,{}};page.origin={scope,seed["bookmarkGuid"],int64_t(root.children.size()+10)};
            if (seed["title"]=="toolbar") page.metadata.tags={"Library tag"};
            folder.children.push_back(std::move(page));root.children.push_back(std::move(folder));
        }
        BookmarkNode library{0,"Library "+scope,"place:type=9",false,0,{}};library.origin={scope,"library_____",90};
        root.children[1].children.push_back(std::move(library));return root;
    };
    CHECK(profile.bookmarks.Import(build("first-library"),"First").Changed());
    const auto id=Find(profile.bookmarks,"first-library","library_____")->id;
    const BookmarkOrigin origin=profile.bookmarks.Find(id)->origin;
    for (const auto& seed:fixture["seed"]) {
        const uint32_t type=seed["transition"];PageRecord page{seed["url"],seed["title"],0,false,type==7?0:1};
        page.visitCountKnown=true;page.visitTransitions={{seed["time"],type}};
        SetHistoryVisitTimes(page,{seed["time"].get<int64_t>()});profile.history.push_back(std::move(page));
    }
    const auto now=fixture["base"].get<int64_t>()+3600000000;
    auto query=[&](const std::string& url) {return QueryBookmarks(url,profile.bookmarks,profile.history,origin,now);};
    size_t compared=0,unsupported=0;
    for (const auto& test:fixture["queries"]) {
        const std::string url=test["uri"];const auto actual=query(url);
        if (test.contains("error") || (test["rows"].empty() && url.find("excludeQueries=1")==std::string::npos)
            || url.find("includeHidden=1")!=std::string::npos || url.find("onlyBookmarked=1")!=std::string::npos
            || url.find("&OR")!=std::string::npos) {
            ++unsupported;CHECK(!actual.error.empty() && actual.items.empty());continue;
        }
        ++compared;CHECK(actual.error.empty() && actual.visitTimes.empty());
        auto expected=json::array(),rows=json::array();
        for (const auto& node:test["rows"]) expected.push_back({node["title"],node["url"]});
        for (const auto& node:actual.items) {
            CHECK(node.id==0 && !node.folder && node.children.empty() && node.origin.scope==origin.scope);
            rows.push_back({node.title,node.url});
        }
        if (rows!=expected) {std::cerr<<url<<"\nActual "<<rows<<"\nExpected "<<expected<<'\n';CHECK(false);}else CHECK(true);
    }
    CHECK(compared+unsupported==41 && compared==29); // Includes both frecency sorts (21/22).
    CHECK(query("place:type=9&expandQueries=0").expandQueries==false);
    CHECK(query("place:type=7").items.empty() && !query("place:type=7").error.empty());
    const auto snapshot=query("place:type=9");CHECK(snapshot.items.size()==4);
    for (unsigned section=1;section<=4;++section) {
        auto target=ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,id,section);
        CHECK(target.error.empty() && target.node.url==snapshot.items[section-1].url && target.node.origin.scope==origin.scope);
        CHECK(BookmarkQuerySectionForURL(target.node.url)==section);
        const auto results=QueryBookmarks(target.node.url,profile.bookmarks,profile.history,target.node.origin,now);
        CHECK(results.error.empty() && !results.items.empty());
        if (section==2) CHECK(results.items.size()==1 && results.items[0].url==fixture["seed"][1]["url"].get<std::string>());
        if (section==3) CHECK(results.items[0].title=="Library tag" && results.items[0].children.size()==1);
        if (section==4) CHECK(results.items.size()==3 && results.items[0].children[0].url==fixture["seed"][0]["url"].get<std::string>());
    }
    CHECK(!ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,id,5).error.empty());
    CHECK(BookmarkQuerySectionForURL("place:type=9")==0);
    // The four links carry a saved parent plus stable section, never id=0.
    const auto html=RenderBookmarkQueryPage(profile.bookmarks,profile.history,id,{},"a123");
    for (unsigned section=1;section<=4;++section) {
        const auto link="summit-bookmarks:query?token=a123&id="+std::to_string(id)+"&section="+std::to_string(section);
        const auto parsed=ParseBookmarkPageLink(link);CHECK(parsed && parsed->id==id && parsed->section==section);
        CHECK(html.find("&amp;id="+std::to_string(id)+"&amp;section="+std::to_string(section))!=std::string::npos);
        const auto child=RenderBookmarkQueryPage(profile.bookmarks,profile.history,id,{},"a123",section);
        CHECK(child.find("Back to Library first-library")!=std::string::npos);
        CHECK(child.find("This smart folder has been removed or changed.")==std::string::npos);
        const auto name=BookmarkQueryPageName(id,section);const auto location=ParseBookmarkQueryPageName(name);
        CHECK(location && location->id==id && location->section==section);
    }
    const auto plain=ParseBookmarkQueryPageName(BookmarkQueryPageName(id));CHECK(plain && plain->id==id && plain->section==0);
    for (const auto* name:{"bookmarks-query-0.html","bookmarks-query--1.html","bookmarks-query-1-0.html","bookmarks-query-1-5.html",
            "bookmarks-query-1-01.html","bookmarks-query-1-2-3.html","bookmarks-query-1-2.html/../x","bookmarks-query-1-2.html?x",
            "bookmarks-query-1-2.htm","bookmarks-query-1000000000000000000.html","other/bookmarks-query-1-2.html"})
        CHECK(!ParseBookmarkQueryPageName(name));
    for (const auto* suffix:{"5","-1","x","4294967296","","%32"})
        CHECK(!ParseBookmarkPageLink("summit-bookmarks:query?token=a123&id=4&section="+std::string(suffix)));
    CHECK(!ParseBookmarkPageLink("summit-bookmarks:query?token=a123&id=0&section=1"));
    CHECK(!ParseBookmarkPageLink("summit-bookmarks:delete?token=a123&id=4&section=1"));
    // Equal query URLs in another import must not change the selected source.
    CHECK(profile.bookmarks.Import(build("second-library",true),"Second").Changed());
    const auto secondID=Find(profile.bookmarks,"second-library","library_____")->id;
    auto second=RenderBookmarkQueryPage(profile.bookmarks,profile.history,secondID,{},"a123",4);
    CHECK(second.find("https://second.library.example/toolbar")!=std::string::npos && second.find("https://library.example/toolbar")==std::string::npos);
    CHECK(profile.bookmarks.Move(Find(profile.bookmarks,"second-library","toolbar_____")->id,kBookmarksBarFolder,kBookmarkEnd));
    second=RenderBookmarkQueryPage(profile.bookmarks,profile.history,secondID,{},"a123",4);
    CHECK(second.find("https://second.library.example/toolbar")!=std::string::npos);
    CHECK(profile.bookmarks.Edit(secondID,"Reordered library","place:type=9&sort=2"));
    CHECK(ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,2).node.url=="place:transition=7&sort=4");
    CHECK(profile.bookmarks.Edit(secondID,"Limited library","place:type=9&maxResults=1"));
    CHECK(!ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,2).error.empty());
    CHECK(ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,1).error.empty());
    CHECK(RenderBookmarkQueryPage(profile.bookmarks,profile.history,secondID,{},"a123",2).find("no longer contains")!=std::string::npos);
    CHECK(profile.bookmarks.Edit(secondID,"Excluded library","place:type=9&excludeQueries=1"));
    CHECK(!ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,1).error.empty());
    CHECK(profile.bookmarks.Edit(secondID,"Changed","https://changed.library.example/"));
    CHECK(!ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,4).error.empty());
    CHECK(profile.bookmarks.Remove(secondID));
    CHECK(!ResolveBookmarkQueryTarget(profile.bookmarks,profile.history,secondID,4).error.empty());
    // HTML without source identities cannot substitute the native roots.
    BookmarkNode imported;std::string error;
    CHECK(ParseNetscapeBookmarks("<!DOCTYPE NETSCAPE-Bookmark-file-1><DL><p><DT><A HREF=\"place:type=9\">HTML library</A></DL><p>",imported,error));
    Bookmarks plainHTML;CHECK(plainHTML.Import(imported,"HTML").Changed());
    const auto htmlID=plainHTML.FindURL("place:type=9")->id;
    CHECK(RenderBookmarkQueryPage(plainHTML,{},htmlID,{},"a123",4).find("target is missing")!=std::string::npos);
    // Saving and exporting retain only the actual queries, not derived links.
    const auto dir=std::filesystem::temp_directory_path()/("summit-sidebar-tests-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);CHECK(profile.Save(dir/"profile.json",error));
    const auto loaded=Profile::Load(dir/"profile.json",error);CHECK(error.empty());
    const auto restored=Find(loaded.bookmarks,"first-library","library_____");CHECK(restored && restored->children.empty());
    CHECK(restored && ResolveBookmarkQueryTarget(loaded.bookmarks,loaded.history,restored->id,4).node.origin.scope==origin.scope);
    const auto exported=WriteNetscapeBookmarks(profile.bookmarks);
    CHECK(exported.find("place:type=9")!=std::string::npos && exported.find("place:transition=7")==std::string::npos);
    CHECK(json::parse(std::ifstream(dir/"profile.json")).dump().find("querySection")==std::string::npos);
    std::filesystem::remove_all(dir);
    std::cout<<checks<<" sidebar query checks, "<<failures<<" failures; "<<compared<<" current comparisons, "<<unsupported<<" explicit explanations\n";
    return failures?1:0;
}
