#include "core/BookmarkIO.h"
#include "core/BookmarkQueries.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
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


static std::string Titles(const BookmarkQueryResult& result)
{
    std::string text;
    for (const auto& node : result.items) text += (text.empty() ? "" : ",") + node.title;
    return text;
}
int main()
{
    constexpr int64_t second = 1000000;
    const auto directory = std::filesystem::temp_directory_path() / ("summit-modified-queries-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    Bookmarks marks;
    const auto ancestor = marks.AddFolder(kBookmarksBarFolder, "Ancestor", kBookmarkEnd, 1);
    const auto folder = marks.AddFolder(ancestor, "Folder", kBookmarkEnd, 2);
    const auto target = marks.AddFolder(kOtherBookmarksFolder, "Target", kBookmarkEnd, 3);
    const auto a = marks.Add({"https://a.test/", "A"}, folder, kBookmarkEnd, 4);
    const auto b = marks.Add({"https://b.test/", "B"}, folder, kBookmarkEnd, 5);
    const auto sep = marks.AddSeparator(folder, kBookmarkEnd, 6);
    CHECK(marks.Find(a)->modified == 4 * second && marks.Find(sep)->modified == 6 * second);
    CHECK(marks.Find(folder)->modified == 6 * second && marks.Find(ancestor)->modified == 6 * second && marks.Bar().modified == 6 * second);
    const auto before = HistoryNowMicroseconds();
    CHECK(marks.Edit(a, "Edited A", "https://a.test/"));
    CHECK(marks.Find(a)->modified >= before && marks.Find(a)->modified <= HistoryNowMicroseconds());
    CHECK(marks.Find(a)->modified == marks.Find(folder)->modified && marks.Find(a)->modified == marks.Find(ancestor)->modified);
    CHECK(marks.Find(b)->modified == 5 * second && marks.Find(sep)->modified == 6 * second);
    auto edited = marks.Find(a)->modified;
    CHECK(!marks.Edit(a, "Edited A", "https://a.test/") && marks.Find(a)->modified == edited);
    CHECK(!marks.Edit(a, "Bad", "data:text/plain,bad") && marks.Find(a)->modified == edited);
    CHECK(!marks.Move(ancestor, folder, 0) && marks.Find(ancestor)->modified == edited);
    CHECK(!marks.Move(a, folder, 1) && marks.Find(a)->modified == edited);
    CHECK(marks.SetMetadata(a, {"Description", {"work"}}));
    edited = marks.Find(a)->modified;
    CHECK(!marks.SetMetadata(a, {"Description", {"work"}}) && marks.Find(a)->modified == edited);
    CHECK(marks.Move(a, target, kBookmarkEnd));
    CHECK(marks.Find(a)->modified == marks.Find(target)->modified && marks.Find(a)->modified == marks.Find(folder)->modified);
    CHECK(marks.Bar().modified == marks.Other().modified && marks.Find(b)->modified == 5 * second);
    CHECK(marks.Move(sep, target, 0) && marks.Find(sep)->modified == marks.Find(target)->modified);
    CHECK(marks.Remove(sep) && marks.Find(target)->modified >= edited);
    CHECK(marks.RemoveURL("https://b.test/") && marks.Find(folder)->modified == marks.Bar().modified);
    auto unchanged = marks.Bar().modified;
    CHECK(!marks.RemoveURL("https://missing.test/") && !marks.Remove(999999) && marks.Bar().modified == unchanged);

    BookmarkNode source; source.folder = true;
    source.children = {{0,"Zed","https://zed.test/",false,10,{}},{0,"Alpha","https://alpha.test/",false,10,{}},
        {0,"Unknown","https://unknown.test/",false,0,{}},{0,"Ascending","place:queryType=1&sort=13",false,0,{}},
        {0,"Shortcut","place:parent=toolbar_____",false,0,{}}};
    source.children[0].modified = 20 * second + 2;
    source.children[1].modified = 20 * second + 1;
    source.children[3].modified = 999 * second;
    Bookmarks imported;
    auto result = imported.Import(source, "Imported", 100);
    CHECK(result.added == 5 && result.updated == 0);
    CHECK(imported.FindURL("https://zed.test/")->modified == 20 * second + 2);
    CHECK(imported.FindURL("https://unknown.test/")->modified == 0);
    CHECK(Titles(QueryBookmarks("place:queryType=1&sort=13", imported, {})) == "Unknown,Alpha,Zed");
    CHECK(Titles(QueryBookmarks("place:queryType=1&sort=14&maxResults=2", imported, {})) == "Zed,Alpha");
    CHECK(QueryBookmarks("place:queryType=1&excludeItems=1", imported, {}).items.empty());
    CHECK(QueryBookmarks("place:queryType=1", imported, {}).items.size() == 3);
    CHECK(QueryBookmarks("place:type=6&sort=14", imported, {}).items.empty() && !QueryBookmarks("place:type=6&sort=14", imported, {}).error.empty());
    unchanged = imported.Other().modified;
    CHECK(!imported.Import(source, "Imported", 200).Changed() && imported.Other().modified == unchanged);
    const auto unknown = imported.FindURL("https://unknown.test/")->id;
    source.children[2].modified = 25 * second + 7;
    result = imported.Import(source, "Imported", 200);
    CHECK(result.updated == 1 && result.added == 0 && imported.Find(unknown)->modified == 25 * second + 7);
    CHECK(imported.Other().modified == unchanged); // Filling a missing date does not fabricate a content edit.
    CHECK(!imported.Import(source, "Imported", 200).Changed());
    source.children[0].metadata.description = "New imported description";
    CHECK(imported.Import(source, "Imported", 300).updated == 1);
    CHECK(imported.FindURL("https://zed.test/")->modified == 300 * second);
    CHECK(imported.Other().modified == 300 * second);
    CHECK(imported.Edit(unknown, "Local edit", "https://unknown.test/"));
    edited = imported.Find(unknown)->modified;
    CHECK(!imported.Import(source, "Imported", 400).Changed() && imported.Find(unknown)->modified == edited);
    CHECK(Titles(QueryBookmarks("place:queryType=1&sort=14&maxResults=1", imported, {})) == "Local edit");

    std::string error;
    const auto html = WriteNetscapeBookmarks(imported);
    CHECK(html.find("LAST_MODIFIED=\"20\" SUMMIT_LAST_MODIFIED_US=\"20000001\"") != std::string::npos);
    BookmarkNode fromHTML;
    CHECK(ParseNetscapeBookmarks(html, fromHTML, error));
    Bookmarks roundtrip;
    CHECK(roundtrip.Import(fromHTML, "HTML").added == 5);
    CHECK(roundtrip.FindURL("https://alpha.test/")->modified == 20 * second + 1);
    CHECK(roundtrip.FindURL("https://unknown.test/")->modified == edited);
    CHECK(!roundtrip.Import(fromHTML, "HTML").Changed());
    BookmarkNode malformed;
    CHECK(ParseNetscapeBookmarks("<DL><A HREF='https://fallback.test/' LAST_MODIFIED='10' SUMMIT_LAST_MODIFIED_US='9223372036854775808'>Fallback</A><HR LAST_MODIFIED='11'><H3 LAST_MODIFIED='12'>Folder</H3><DL></DL></DL>", malformed, error));
    CHECK(malformed.children[0].modified == 10 * second && malformed.children[1].modified == 11 * second && malformed.children[2].modified == 12 * second);
    CHECK(ParseNetscapeBookmarks("<DL><A HREF='https://unknown.test/' LAST_MODIFIED='-1' SUMMIT_LAST_MODIFIED_US='nan'>Unknown</A></DL>", malformed, error));
    CHECK(malformed.children[0].modified == 0);
    BookmarkNode dividers; dividers.folder = true;
    dividers.children = {{0,"Same","",false,0,{},true},{0,"Same","",false,0,{},true}};
    Bookmarks separatorDates;
    auto separated = separatorDates.Import(dividers,"Dividers",100);
    CHECK(separated.separatorsAdded == 2);
    dividers.children[0].modified = 7000001; dividers.children[1].modified = 7000002;
    CHECK(separatorDates.Import(dividers,"Dividers",200).updated == 2);
    CHECK(separatorDates.Find(separated.folder)->children[0].modified == 7000001
        && separatorDates.Find(separated.folder)->children[1].modified == 7000002);
    CHECK(!separatorDates.Import(dividers,"Dividers",300).Changed());
    BookmarkNode chrome;
    CHECK(ParseChromeBookmarks(R"({"roots":{"bookmark_bar":{"name":"Bar","date_modified":"11644473610000123","children":[{"name":"Folder","type":"folder","date_modified":"11644473620000456","children":[{"name":"A","type":"url","url":"https://chrome.test/","date_modified":"11644473630000789"}]}]}}})",chrome,error));
    CHECK(chrome.children[0].modified == 10 * second + 123 && chrome.children[0].children[0].modified == 20 * second + 456);
    CHECK(chrome.children[0].children[0].children[0].modified == 30 * second + 789);

    std::string library = "libsqlite3.so.0";
    if (const char* value = std::getenv("SUMMIT_TEST_SQLITE")) library = value;
    const auto database = directory / "places.sqlite";
    CHECK(SQL(library,database,R"(
      CREATE TABLE moz_places(id INTEGER PRIMARY KEY,url TEXT);
      CREATE TABLE moz_bookmarks(id INTEGER PRIMARY KEY,type INTEGER,fk INTEGER,parent INTEGER,position INTEGER,title TEXT,dateAdded INTEGER,guid TEXT,lastModified INTEGER);
      INSERT INTO moz_places VALUES(1,'https://sqlite.test/a'),(2,'https://sqlite.test/b'),(3,'place:parent=toolbar_____&sort=14');
      INSERT INTO moz_bookmarks VALUES(1,2,NULL,0,0,'Root',1000000,'root________',2000001),
        (2,2,NULL,1,0,'Toolbar',3000000,'toolbar_____',4000002),
        (10,1,1,2,0,'Zed',5000000,'pagea_______',6000003),
        (11,1,2,2,1,'Alpha',5000000,'pageb_______',6000003),
        (12,3,NULL,2,2,'Divider',5000000,'separator___',6000004),
        (13,1,3,2,3,'Sorted',5000000,'sorted______',6000005);
    )"));
    BookmarkNode sqlite;
    CHECK(ReadFirefoxPlaces(database,library,&sqlite,nullptr,error));
    CHECK(error.empty() && sqlite.modified == 2000001 && sqlite.children[0].modified == 4000002);
    CHECK(sqlite.children[0].children[0].modified == 6000003 && sqlite.children[0].children[2].modified == 6000004);
    Profile profile;
    CHECK(profile.bookmarks.Import(sqlite,"Firefox",100).added == 3);
    auto& stored=profile.bookmarks;
    auto shortcut=stored.FindURL("place:parent=toolbar_____&sort=14");
    const auto shortcutID=shortcut->id;
    CHECK(Titles(QueryBookmarks(shortcut->url,stored,{},shortcut->origin)) == "Sorted,Zed,Alpha");
    CHECK(Titles(QueryBookmarks("place:queryType=1&sort=13",stored,{})) == "Alpha,Zed");
    const auto folderID=stored.Parent(stored.FindURL("https://sqlite.test/a")->id)->id;
    CHECK(stored.Find(folderID)->modified == 4000002);
    CHECK(!stored.Import(sqlite,"Firefox",200).Changed() && stored.Find(folderID)->modified == 4000002);
    CHECK(stored.Edit(stored.FindURL("https://sqlite.test/a")->id,"Changed A","https://sqlite.test/a"));
    edited=stored.FindURL("https://sqlite.test/a")->modified;
    CHECK(stored.Move(stored.FindURL("https://sqlite.test/a")->id,kBookmarksBarFolder,kBookmarkEnd));
    CHECK(!stored.Import(sqlite,"Firefox",300).Changed());
    CHECK(stored.FindURL("https://sqlite.test/a")->modified >= edited && stored.OnBar("https://sqlite.test/a"));
    CHECK(profile.Save(directory/"profile.json",error));
    auto loaded=Profile::Load(directory/"profile.json",error);
    CHECK(error.empty() && loaded.bookmarks.FindURL("https://sqlite.test/a")->modified == stored.FindURL("https://sqlite.test/a")->modified);
    CHECK(loaded.bookmarks.Bar().modified == stored.Bar().modified && loaded.bookmarks.Other().modified == stored.Other().modified);
    CHECK(loaded.bookmarks.FindURL("https://sqlite.test/b")->modified == 6000003);
    const auto page=RenderBookmarkQueryPage(stored,{},shortcutID,{},"test");
    CHECK(page.find("Alpha") != std::string::npos && page.find("Changed A") == std::string::npos);
    auto json=nlohmann::json::parse(std::ifstream(directory/"profile.json"));
    json["bookmarkTree"]["bar"][0]["modified"]=uint64_t(-1);
    std::ofstream(directory/"invalid.json")<<json;
    auto invalid=Profile::Load(directory/"invalid.json",error);
    CHECK(error.empty() && invalid.bookmarks.Bar().children[0].modified == 0);
    CHECK(SQL(library,database,"UPDATE moz_bookmarks SET lastModified=NULL WHERE id=1"));
    BookmarkNode unknownRoot;
    CHECK(ReadFirefoxPlaces(database,library,&unknownRoot,nullptr,error) && unknownRoot.modified == 0);
    Bookmarks unknownDates;
    auto unknownImport = unknownDates.Import(unknownRoot,"Firefox",100);
    CHECK(unknownImport.folder && unknownDates.Find(unknownImport.folder)->modified == 0);
    std::filesystem::remove_all(directory);
    std::cout<<checks<<" bookmark modification query checks, "<<failures<<" failures\n";
    return failures?1:0;
}
