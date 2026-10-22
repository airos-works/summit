#include "core/BookmarkQueries.h"
#include "core/BookmarkIO.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <unistd.h>
using namespace summit;
using nlohmann::json;
static int checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
static const BookmarkNode* Find(const BookmarkNode& node,const std::string& scope,const std::string& guid)
{
    if (node.origin.scope==scope && node.origin.guid==guid) return &node;
    for (const auto& child:node.children) if (const auto* found=Find(child,scope,guid)) return found;
    return nullptr;
}
static const BookmarkNode* Find(const Bookmarks& marks,const std::string& scope,const std::string& guid)
{
    if (const auto* found=Find(marks.Bar(),scope,guid)) return found;
    return Find(marks.Other(),scope,guid);
}
static json Tree(const std::vector<BookmarkNode>& nodes)
{
    auto tree=json::array();
    for (const auto& node:nodes) {
        CHECK(node.folder ? node.id==0 : node.id>0);
        if (node.folder) tree.push_back({{"title",node.title},{"children",Tree(node.children)}});
        else if (node.separator) tree.push_back({{"separator",true}});
        else tree.push_back({{"title",node.title},{"url",node.url}});
    }
    return tree;
}
static json Oracle(const json& nodes)
{
    auto tree=json::array();
    for (const auto& node:nodes) {
        if (node.contains("children")) tree.push_back({{"title",node["title"]},{"children",Oracle(node["children"])}});
        else if (node["type"]==7) tree.push_back({{"separator",true}});
        else tree.push_back({{"title",node["title"]},{"url",node["uri"]}});
    }
    return tree;
}
int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    json fixture;std::ifstream(argv[1])>>fixture;
    Profile profile;
    const std::string scope="root-fixture";
    auto build=[&](const std::string& source) {
        BookmarkNode tree{0,"Import","",true,0,{}};
        tree.origin={source,"root________",1};
        std::map<std::string,int64_t> ids;
        int64_t id=10;
        for (const auto& seed:fixture["seed"]) ids[seed["guid"]]=id++;
        std::function<void(BookmarkNode&)> children=[&](auto& parent) {
            for (const auto& seed:fixture["seed"]) if (seed["parent"]==parent.origin.guid) {
                BookmarkNode node{0,seed["title"],seed["url"],seed["type"]==2,seed["added"].get<int64_t>()/1000000,{}};
                node.separator=seed["type"]==3;node.modified=seed["modified"];
                node.origin={source,seed["guid"],ids[seed["guid"]]};
                if (fixture["tags"].contains(node.url)) node.metadata.tags=fixture["tags"][node.url].get<std::vector<std::string>>();
                if (node.folder) children(node);
                parent.children.push_back(std::move(node));
            }
        };
        int64_t rootID=2;
        for (const auto& [guid,title]:{std::pair{"toolbar_____","Imported toolbar"},{"menu________","Imported menu"},{"unfiled_____","Imported other"},{"mobile______","Imported mobile"}}) {
            BookmarkNode node{0,title,"",true,0,{}};node.origin={source,guid,rootID++};children(node);tree.children.push_back(std::move(node));
        }
        return tree;
    };
    auto imported=build(scope);
    CHECK(profile.bookmarks.Import(imported,"Fixture").Changed());
    for (const auto& seed:fixture["history"]) {
        PageRecord page{seed["url"],seed["title"],0,false,int(seed["times"].size())};
        SetHistoryVisitTimes(page,seed["times"].get<std::vector<int64_t>>());profile.history.push_back(std::move(page));
    }
    BookmarkOrigin origin{scope,"query_root__",99};
    auto query=[&](const std::string& url) { return QueryBookmarks(url,profile.bookmarks,profile.history,origin); };
    for (const auto& test:fixture["queries"]) {
        if (test["mobile"]==true) continue; // This UI preference is absent from bookmark imports; default false.
        const auto url=test["uri"].get<std::string>();const auto result=query(url);
        if (test.contains("error") || test["tree"]["children"].empty()) {
            CHECK(!result.error.empty() && result.items.empty());continue;
        }
        CHECK(result.error.empty() && result.visitTimes.empty());
        auto expected=Oracle(test["tree"]["children"]),actual=Tree(result.items);
        if (actual!=expected) {
            std::cerr<<"Firefox root mismatch: "<<url<<'\n'<<"Actual: "<<actual.dump()<<'\n'<<"Expected: "<<expected.dump()<<'\n';CHECK(false);
        } else CHECK(true);
    }
    auto before=query("place:type=8");CHECK(before.items.size()==3);
    CHECK(before.items[0].title=="Bookmarks Toolbar" && before.items[1].title=="Bookmarks Menu" && before.items[2].title=="Other Bookmarks");
    const auto toolbar=Find(profile.bookmarks,scope,"toolbar_____")->id;
    CHECK(before.items[0].id==0 && before.items[0].origin.scope==scope);
    CHECK(profile.bookmarks.Edit(toolbar,"Renamed imported toolbar",""));
    CHECK(query("place:type=8").items[0].title=="Bookmarks Toolbar"); // Virtual title is not the stored root title.
    CHECK(profile.bookmarks.Move(toolbar,kBookmarksBarFolder,kBookmarkEnd));
    CHECK(query("place:type=8").items[0].origin==profile.bookmarks.Find(toolbar)->origin);
    const auto added=profile.bookmarks.Add({"https://new.root.test/","New page"},toolbar);
    CHECK(query("place:type=8").items[0].children.size()==before.items[0].children.size()+1);
    CHECK(before.items[0].children.size()==5); // Earlier result remains an immutable value.
    CHECK(profile.bookmarks.Remove(added));
    auto second=build("second-root-fixture");
    second.children[0].children[0].title="Only second source";
    CHECK(profile.bookmarks.Import(second,"Second source").Changed());
    const auto secondResult=QueryBookmarks("place:type=8",profile.bookmarks,profile.history,{"second-root-fixture","query_root__",99});
    CHECK(secondResult.items[0].children[0].title=="Only second source" && query("place:type=8").items[0].children[0].title!="Only second source");
    auto native=QueryBookmarks("place:type=8",profile.bookmarks,profile.history);
    CHECK(native.items.size()==2 && native.items[0].id==0 && native.items[1].id==0);
    CHECK(native.items[0].title==profile.bookmarks.Bar().title && native.items[1].title==profile.bookmarks.Other().title);
    CHECK(query("place:type=8&expandQueries=0").expandQueries==false);
    CHECK(IsBookmarkFolderQueryURL("place:parent=toolbar_____") && !IsBookmarkFolderQueryURL("place:type=8"));
    CHECK(!IsBookmarkFolderQueryURL("place:parent=toolbar_____&terms=x") && !IsBookmarkFolderQueryURL("https://example.test/"));
    CHECK(query("place:type=8&OR").items.size()==3);
    for (const auto* suffix:{"&terms=x","&parent=toolbar_____","&folder=TOOLBAR","&minVisits=1","&beginTimeRef=1","&domain=x","&tag=x","&OR&uri=https%3A%2F%2Fx.test%2F","&annotation=x","&transition=1","&sort=22"}) {
        const auto bad=query(std::string("place:type=8")+suffix);CHECK(!bad.error.empty() && bad.items.empty());
    }
    // Persist and export the source query and identities, never its expansion.
    BookmarkNode saved{0,"All source bookmarks","place:type=8&sort=1",false,0,{}};saved.origin=origin;
    BookmarkNode addition{0,"","",true,0,{saved}};
    CHECK(profile.bookmarks.Import(addition,"Saved query").added==1);
    const auto savedID=Find(profile.bookmarks,scope,"query_root__")->id;
    const auto html=RenderBookmarkQueryPage(profile.bookmarks,profile.history,savedID,{},"root-tests");
    CHECK(html.find("<h2>Bookmarks Toolbar</h2>")!=std::string::npos && html.find("<h3>Root 0 folder</h3>")!=std::string::npos);
    CHECK(html.find("Saved history query")!=std::string::npos && html.find("summit-bookmarks:query?")!=std::string::npos);
    CHECK(profile.bookmarks.Find(savedID)->children.empty());
    BookmarkNode parsed;std::string error;
    CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(profile.bookmarks),parsed,error));
    Bookmarks restored;CHECK(restored.Import(parsed,"HTML").Changed());
    CHECK(Tree(QueryBookmarks("place:type=8",restored,profile.history,origin).items)==Tree(query("place:type=8").items));
    const auto directory=std::filesystem::temp_directory_path()/("summit-root-query-"+std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    CHECK(profile.Save(directory/"profile.json",error));
    const auto loaded=Profile::Load(directory/"profile.json",error);CHECK(error.empty());
    CHECK(Tree(QueryBookmarks("place:type=8",loaded.bookmarks,loaded.history,origin).items)==Tree(query("place:type=8").items));
    std::filesystem::remove_all(directory);
    // Missing/ambiguous roots cannot leak into another imported source.
    const auto menu=Find(profile.bookmarks,scope,"menu________")->id;
    CHECK(profile.bookmarks.Remove(menu));
    auto missing=query("place:type=8");CHECK(missing.items.empty() && missing.error.find("menu________")!=std::string::npos);
    Bookmarks empty;auto emptyResult=QueryBookmarks("place:type=8",empty,{});
    CHECK(emptyResult.items.size()==2 && emptyResult.items[0].children.empty() && emptyResult.items[1].children.empty());
    auto ambiguous=build("ambiguous");ambiguous.children.push_back(ambiguous.children[0]);
    Bookmarks duplicate;duplicate.Assign({},ambiguous); // A loaded tree can contain conflicting imported identities.
    auto ambiguousResult=QueryBookmarks("place:type=8",duplicate,{}, {"ambiguous","query_root__",99});
    CHECK(ambiguousResult.items.empty() && !ambiguousResult.error.empty());
    std::cout<<checks<<" root query checks, "<<failures<<" failures\n";return failures?1:0;
}
