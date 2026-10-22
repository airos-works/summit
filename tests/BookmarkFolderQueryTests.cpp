#include "core/BookmarkIO.h"
#include "core/BookmarkQueries.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <dlfcn.h>
#include <filesystem>
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
static const BookmarkNode* Find(const BookmarkNode& node, const std::string& scope, const std::string& guid)
{
    if (node.origin.scope == scope && node.origin.guid == guid) return &node;
    for (const auto& child : node.children) if (const auto* found = Find(child, scope, guid)) return found;
    return nullptr;
}
static const BookmarkNode* Find(const Bookmarks& marks, const std::string& scope, const std::string& guid)
{
    if (const auto* found = Find(marks.Bar(), scope, guid)) return found;
    return Find(marks.Other(), scope, guid);
}
int main()
{
    const auto directory = std::filesystem::temp_directory_path() / ("summit-folder-queries-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    std::string library = "libsqlite3.so.0";
    if (const char* value = std::getenv("SUMMIT_TEST_SQLITE")) library = value;
    const auto database = directory / "places.sqlite";
    CHECK(SQL(library, database, R"(
      CREATE TABLE moz_places(id INTEGER PRIMARY KEY, url TEXT);
      CREATE TABLE moz_bookmarks(id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER, parent INTEGER, position INTEGER, title TEXT, dateAdded INTEGER, guid TEXT);
      INSERT INTO moz_places VALUES(1,'https://one.test/a'),(2,'https://one.test/nested'),(3,'https://one.test/unfiled'),
        (4,'place:parent=work________'),(5,'place:folder=10'),(6,'place:parent=work________&terms=Nested'),
        (7,'place:parent=toolbar_____'),(8,'place:folder=UNFILED_BOOKMARKS'),(9,'place:parent=missing_____');
      INSERT INTO moz_bookmarks VALUES
        (1,2,NULL,0,0,'',0,'root________'),(2,2,NULL,1,0,'Menu',0,'menu________'),
        (3,2,NULL,1,1,'Toolbar',0,'toolbar_____'),(4,2,NULL,1,2,'Other',0,'unfiled_____'),(5,2,NULL,1,3,'Mobile',0,'mobile______'),
        (10,2,NULL,2,0,'Work',0,'work________'),(11,2,NULL,3,0,'Also Work',0,'otherwork___'),
        (12,2,NULL,10,2,'Nested',0,'nested______'),(13,2,NULL,2,1,'Empty',0,'empty_______'),
        (20,1,1,10,0,'A',0,'pagea_______'),(21,3,NULL,10,1,'Divider',0,'separator___'),
        (22,1,1,11,0,'A elsewhere',0,'pagea2______'),(23,1,2,12,0,'Nested page',0,'pageb_______'),
        (24,1,4,2,2,'Work shortcut',0,'querywork___'),(25,1,5,2,3,'Legacy shortcut',0,'querylegacy_'),
        (26,1,6,2,4,'Nested search',0,'querysearch_'),(27,1,7,2,5,'Toolbar shortcut',0,'querytoolbar'),
        (28,1,8,2,6,'Other shortcut',0,'queryother__'),(29,1,9,2,7,'Missing shortcut',0,'querymissing'),
        (30,1,4,10,3,'Self shortcut',0,'queryself___'),(31,1,3,4,0,'Unfiled',0,'pagec_______');
    )"));
    if (failures) return 1;
    BookmarkNode imported;
    std::string error;
    CHECK(ReadFirefoxPlaces(database, library, &imported, nullptr, error));
    CHECK(error.empty() && imported.children.size() == 4 && !imported.origin.Empty());
    if (failures) return 1;
    const auto scope = imported.origin.scope;
    CHECK(scope.find(directory.string()) == std::string::npos && scope.size() == 72);
    Profile profile; auto& marks = profile.bookmarks;
    const auto first = marks.Import(imported, "Firefox");
    CHECK(first.added == 11 && first.separatorsAdded == 1);
    CHECK(marks.Count() == 11 && Find(marks, scope, "empty_______") && Find(marks, scope, "mobile______"));
    auto query = [&](const std::string& guid) {
        const auto* node = Find(marks, scope, guid);
        return node ? QueryBookmarks(node->url, marks, profile.history, node->origin) : BookmarkQueryResult{{}, "missing query"};
    };
    auto direct = query("querywork___");
    CHECK(direct.error.empty() && direct.items.size() == 4);
    if (failures) return 1;
    CHECK(direct.items[0].url == "https://one.test/a" && direct.items[1].separator);
    CHECK(direct.items[2].folder && direct.items[2].children.size() == 1);
    CHECK(direct.items[3].url == "place:parent=work________");
    CHECK(query("querylegacy_").items.size() == 4);
    CHECK(query("querysearch_").items.size() == 1 && query("querysearch_").items[0].url == "https://one.test/nested");
    CHECK(query("querytoolbar").items.size() == 1 && query("querytoolbar").items[0].folder);
    CHECK(query("queryother__").items.size() == 1 && query("queryother__").items[0].url == "https://one.test/unfiled");
    CHECK(query("querymissing").items.empty() && query("querymissing").error.find("target is missing") != std::string::npos);
    const auto origin = Find(marks, scope, "querywork___")->origin;
    auto constrained = [&](const std::string& url) { return QueryBookmarks(url, marks, {}, origin); };
    CHECK(constrained("place:parent=empty_______").error.empty() && constrained("place:parent=empty_______").items.empty());
    CHECK(constrained("place:parent=work________&excludeItems=1").items.size() == 2);
    CHECK(constrained("place:parent=work________&excludeQueries=1").items.size() == 4);
    CHECK(constrained("place:parent=work________&maxResults=1&excludeQueries=1").items.size() == 1);
    CHECK(constrained("place:parent=work________&parent=otherwork___&terms=one.test/a").items.size() == 2);
    CHECK(constrained("place:folder=BOOKMARKS_MENU").items.size() == Find(marks, scope, "menu________")->children.size());
    CHECK(constrained("place:folder=PLACES_ROOT").items.size() == 4);
    for (const char* invalid : {"place:parent=", "place:folder=", "place:folder=-1", "place:folder=10junk", "place:folder=999999999999999999999"})
        CHECK(!constrained(invalid).error.empty() && constrained(invalid).items.empty());
    CHECK(!marks.Import(imported, "Firefox").Changed());
    const auto workID = Find(marks, scope, "work________")->id;
    CHECK(marks.Edit(workID, "Renamed", ""));
    CHECK(marks.Move(workID, kBookmarksBarFolder, kBookmarkEnd));
    CHECK(marks.Move(Find(marks, scope, "pagea_______")->id, Find(marks, scope, "otherwork___")->id, kBookmarkEnd));
    CHECK(query("querywork___").items.size() == 3 && query("querylegacy_").items.size() == 3);
    CHECK(marks.Move(Find(marks, scope, "nested______")->id, kOtherBookmarksFolder, kBookmarkEnd));
    CHECK(query("querysearch_").items.empty());
    CHECK(!marks.Import(imported, "Firefox").Changed());
    CHECK(Find(marks, scope, "work________")->title == "Renamed" && marks.Parent(workID)->id == kBookmarksBarFolder);
    CHECK(query("querywork___").items.size() == 2 && marks.Count() == 11);
    const auto newPage = marks.Add({"https://one.test/new", "New live member"}, workID);
    CHECK(newPage && query("querywork___").items.size() == 3);
    CHECK(marks.Remove(newPage) && query("querywork___").items.size() == 2);
    const auto path = directory / "profile.json";
    CHECK(profile.Save(path, error));
    auto restored = Profile::Load(path, error);
    CHECK(error.empty() && Find(restored.bookmarks, scope, "work________"));
    const auto* savedQuery = Find(restored.bookmarks, scope, "querylegacy_");
    CHECK(savedQuery && QueryBookmarks(savedQuery->url, restored.bookmarks, {}, savedQuery->origin).items.size() == 2);
    const auto html = WriteNetscapeBookmarks(marks);
    CHECK(html.find("SUMMIT_ORIGIN=") != std::string::npos && html.find("place:folder=10") != std::string::npos);
    BookmarkNode exported;
    CHECK(ParseNetscapeBookmarks(html, exported, error));
    Bookmarks roundtrip;
    CHECK(roundtrip.Import(exported, "Export").added == 11);
    CHECK(!roundtrip.Import(exported, "Export").Changed());
    savedQuery = Find(roundtrip, scope, "querylegacy_");
    CHECK(savedQuery && QueryBookmarks(savedQuery->url, roundtrip, {}, savedQuery->origin).items.size() == 2);
    CHECK(Find(roundtrip, scope, "separator___") && Find(roundtrip, scope, "empty_______"));
    // A second database deliberately reuses every GUID and numeric ID.
    const auto second = directory / "second.sqlite";
    std::filesystem::copy_file(database, second);
    CHECK(SQL(library, second, "UPDATE moz_places SET url='https://two.test/a' WHERE id=1"));
    BookmarkNode imported2;
    CHECK(ReadFirefoxPlaces(second, library, &imported2, nullptr, error));
    CHECK(imported2.origin.scope != scope && marks.Import(imported2, "Firefox").added == 11);
    savedQuery = Find(marks, imported2.origin.scope, "querylegacy_");
    CHECK(savedQuery && QueryBookmarks(savedQuery->url, marks, {}, savedQuery->origin).items[0].url == "https://two.test/a");
    CHECK(query("querylegacy_").items.size() == 2 && marks.Count() == 22);
    CHECK(!marks.Import(imported2, "Firefox").Changed());
    CHECK(marks.SetMetadata(Find(marks, scope, "querylegacy_")->id, {"", {}, {{"folder", "", ""}}}));
    CHECK(marks.ResolveKeyword("folder")->bookmarkID == Find(marks, scope, "querylegacy_")->id);
    CHECK(marks.SetMetadata(savedQuery->id, {"", {}, {{"folder", "", ""}}}));
    CHECK(!marks.ResolveKeyword("folder")->error.empty());
    CHECK(marks.Remove(workID));
    CHECK(query("querywork___").items.empty() && !query("querywork___").error.empty());
    CHECK(QueryBookmarks(savedQuery->url, marks, {}, savedQuery->origin).items.size() == 4);
    // Standard Firefox HTML has no folder identities: preserve the query,
    // but never retarget its toolbar reference to Summit's native toolbar.
    BookmarkNode plain;
    CHECK(ParseNetscapeBookmarks("<DL><DT><A HREF='place:parent=toolbar_____'>Original toolbar</A></DL>", plain, error));
    CHECK(!plain.children[0].origin.Empty());
    CHECK(!QueryBookmarks(plain.children[0].url, marks, {}, plain.children[0].origin).error.empty());
    CHECK(NormalizeBookmarkOrigin({std::string(129, 'a'), "guid", 1}).Empty());
    CHECK(NormalizeBookmarkOrigin({"scope", "bad/guid", 1}).Empty());
    CHECK(NormalizeBookmarkOrigin({"scope", "guid", -1}).Empty());
    CHECK(RenderBookmarkQueryPage(marks, {}, Find(marks, scope, "querywork___")->id, {}, "test").find("target is missing") != std::string::npos);
    BookmarkNode longSeparator{0, "", "", true, 0, {}};
    longSeparator.children.push_back({0, std::string(5000, 's'), "", false, 0, {}, true});
    Bookmarks separators;
    CHECK(separators.Import(longSeparator, "Long separator").separatorsAdded == 1 && separators.SeparatorCount() == 1);
    CHECK(!separators.Import(longSeparator, "Long separator").Changed() && separators.SeparatorCount() == 1);
    std::filesystem::remove_all(directory);
    std::cout << checks << " folder query checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
