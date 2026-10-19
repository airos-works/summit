#include "core/BookmarkIO.h"
#include "core/Bookmarks.h"
#include "core/InternalPages.h"
#include "core/Profile.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)

using namespace summit;

static const BookmarkNode* Child(const BookmarkNode& folder, const std::string& title)
{
    for (const auto& child : folder.children) if (child.title == title) return &child;
    return nullptr;
}

static std::string Titles(const BookmarkNode& folder)
{
    std::string text;
    for (const auto& child : folder.children) text += (text.empty() ? "" : ",") + child.title + (child.folder ? "/" : "");
    return text;
}

static void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

// Makes an SQLite database from SQL with the system's library, as Firefox
// and Chrome would have; false when the library is not installed.
static bool MakeDatabase(const std::string& library, const std::filesystem::path& path, const std::string& sql)
{
    void* handle = dlopen(library.c_str(), RTLD_NOW);
    if (!handle) return false;
    auto open = reinterpret_cast<int (*)(const char*, void**)>(dlsym(handle, "sqlite3_open"));
    auto exec = reinterpret_cast<int (*)(void*, const char*, void*, void*, char**)>(dlsym(handle, "sqlite3_exec"));
    auto close = reinterpret_cast<int (*)(void*)>(dlsym(handle, "sqlite3_close"));
    void* database = nullptr;
    bool done = open && exec && close && open(path.c_str(), &database) == 0 && exec(database, sql.c_str(), nullptr, nullptr, nullptr) == 0;
    if (database) close(database);
    dlclose(handle);
    return done;
}

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / ("summit-bookmark-tests-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);

    // The tree: adding, folders, moving, editing and removing.
    {
        Bookmarks marks;
        CHECK(marks.Empty() && marks.Count() == 0 && marks.FolderCount() == 0);
        const auto a = marks.Add({"https://a.example/", "A"}, kBookmarksBarFolder);
        const auto b = marks.Add({"https://b.example/", "B"}, kBookmarksBarFolder);
        const auto work = marks.AddFolder(kBookmarksBarFolder, "Work");
        const auto c = marks.Add({"https://c.example/", "C"}, work);
        const auto d = marks.Add({"https://d.example/", "D"}, kOtherBookmarksFolder);
        CHECK(a && b && work && c && d && marks.Count() == 4 && marks.FolderCount() == 1);
        CHECK(!marks.Add({"javascript:alert(1)", "J"}, kOtherBookmarksFolder) && !marks.Add({"https://x.example/", "X"}, 999));
        CHECK(!marks.Add({"https://x.example/", "X"}, a));
        CHECK(Titles(marks.Bar()) == "A,B,Work/" && Titles(marks.Other()) == "D");
        // The flat list: the bar's first, those on the bar itself marked.
        const auto& pages = marks.Pages();
        CHECK(pages.size() == 4 && pages[0].url == "https://a.example/" && pages[0].bar && pages[1].bar);
        CHECK(pages[2].url == "https://c.example/" && !pages[2].bar && pages[3].url == "https://d.example/" && !pages[3].bar);
        CHECK(marks.OnBar("https://a.example/") && !marks.OnBar("https://c.example/") && marks.Contains("https://c.example/"));
        CHECK(marks.FolderPath(c) == "Bookmarks Bar › Work" && marks.FolderPath(d) == "Other Bookmarks");
        size_t index = 9;
        CHECK(marks.Parent(c, &index) && marks.Parent(c)->id == work && index == 0);
        // Before the child at an index; the moved node's old place counts.
        CHECK(marks.Move(a, kBookmarksBarFolder, 2) && Titles(marks.Bar()) == "B,A,Work/");
        CHECK(!marks.Move(a, kBookmarksBarFolder, 1) && !marks.Move(a, kBookmarksBarFolder, 2));
        CHECK(marks.Move(a, kBookmarksBarFolder, 0) && Titles(marks.Bar()) == "A,B,Work/");
        CHECK(marks.Move(a, kBookmarksBarFolder, kBookmarkEnd) && Titles(marks.Bar()) == "B,Work/,A");
        CHECK(marks.Move(d, work, 0) && Titles(*marks.Find(work)) == "D,C" && marks.Other().children.empty());
        // Folders cannot go into themselves; the fixed folders cannot move.
        const auto inner = marks.AddFolder(work, "Inner");
        CHECK(inner && !marks.Move(work, inner, 0) && !marks.Move(work, work, 0));
        CHECK(!marks.Move(kBookmarksBarFolder, kOtherBookmarksFolder, 0));
        CHECK(marks.Move(work, kOtherBookmarksFolder, kBookmarkEnd) && Titles(marks.Bar()) == "B,A");
        CHECK(marks.FolderPath(inner) == "Other Bookmarks › Work");
        // Folder choices leave out a folder and what is inside it.
        auto folders = marks.Folders();
        CHECK(folders.size() == 4 && folders[0].id == kBookmarksBarFolder && folders[2].title == "Work" && folders[3].depth == 2);
        folders = marks.Folders(work);
        CHECK(folders.size() == 2 && folders[1].id == kOtherBookmarksFolder);
        CHECK(marks.Edit(c, "See", "https://see.example/") && marks.Find(c)->url == "https://see.example/");
        CHECK(!marks.Edit(c, "See", "https://see.example/") && !marks.Edit(c, "Bad", "javascript:x"));
        CHECK(marks.Edit(work, "Job", "ignored") && marks.Find(work)->title == "Job" && marks.Find(work)->url.empty());
        CHECK(!marks.Edit(kBookmarksBarFolder, "Renamed", ""));
        CHECK(marks.Add({"https://b.example/", "B again"}, inner));
        CHECK(marks.RemoveURL("https://b.example/") && !marks.Contains("https://b.example/") && !marks.RemoveURL("https://b.example/"));
        CHECK(marks.Remove(work) && marks.Count() == 1 && marks.FolderCount() == 0 && !marks.Find(c));
        CHECK(!marks.Remove(kBookmarksBarFolder) && !marks.Remove(12345));
        // Nesting has a limit.
        int64_t parent = kOtherBookmarksFolder;
        for (size_t depth = 1; depth <= kMaxBookmarkDepth; ++depth) parent = marks.AddFolder(parent, "Level");
        CHECK(parent && !marks.AddFolder(parent, "Too deep"));
    }

    // Saved in profile.json, readable by older builds; older profiles load.
    {
        const auto path = directory / "profile.json";
        std::string error;
        Profile profile;
        profile.AddBookmark({"https://bar.example/", "Bar"}, true);
        const auto folder = profile.bookmarks.AddFolder(kBookmarksBarFolder, "Folder", kBookmarkEnd, 1700000000);
        profile.bookmarks.Add({"https://in.example/", "In folder"}, folder, kBookmarkEnd, 1700000001);
        profile.bookmarks.AddFolder(kOtherBookmarksFolder, "Empty");
        profile.AddBookmark({"https://other.example/", "Other"}, false);
        CHECK(profile.Save(path, error));
        nlohmann::json saved;
        { std::ifstream in(path); in >> saved; }
        // Older builds read the flat list: every bookmark, the bar's own marked.
        CHECK(saved["bookmarks"].size() == 3 && saved["bookmarks"][0]["bar"] == true && !saved["bookmarks"][1].contains("bar"));
        CHECK(saved["bookmarks"][1]["url"] == "https://in.example/" && saved["bookmarks"][2]["url"] == "https://other.example/");
        CHECK(saved["bookmarkTree"]["bar"][1]["title"] == "Folder" && saved["bookmarkTree"]["bar"][1]["children"].size() == 1);
        CHECK(saved["bookmarkTree"]["other"][0]["children"].empty() && saved["bookmarkTree"]["bar"][1]["added"] == 1700000000);
        auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && Titles(loaded.bookmarks.Bar()) == "Bar,Folder/" && Titles(loaded.bookmarks.Other()) == "Empty/,Other");
        CHECK(loaded.bookmarks.FindURL("https://in.example/") && loaded.bookmarks.FindURL("https://in.example/")->added == 1700000001);
        CHECK(loaded.bookmarks.Count() == 3 && loaded.bookmarks.FolderCount() == 2);
        // The ids are numbered again and stay unique.
        CHECK(loaded.bookmarks.Find(loaded.bookmarks.FindURL("https://in.example/")->id)->url == "https://in.example/");
        // What an older build saves: the flat list only.
        { std::ofstream out(path); out << R"({"version":1,"tabs":[],"selected":0,"history":[],"bookmarks":[)"
            R"({"url":"https://one.example/","title":"One","bar":true},{"url":"https://two.example/","title":"Two"},)"
            R"({"url":"https://three.example/","title":"Three","bar":true}]})"; }
        loaded = Profile::Load(path, error);
        CHECK(error.empty() && Titles(loaded.bookmarks.Bar()) == "One,Three" && Titles(loaded.bookmarks.Other()) == "Two");
        // Unusable tree entries are dropped instead of failing the profile.
        { std::ofstream out(path); out << R"({"version":1,"tabs":[],"selected":0,"history":[],"bookmarks":[],"bookmarkTree":)"
            R"json({"bar":[{"title":"Bad","url":"javascript:void(0)"},{"title":"Good","url":"https://good.example/"},7,)json"
            R"({"url":"https://untitled.example/"},{"title":"Folder","children":"nonsense"}],"other":{}}})"; }
        loaded = Profile::Load(path, error);
        CHECK(error.empty() && Titles(loaded.bookmarks.Bar()) == "Good,Folder/" && loaded.bookmarks.Other().children.empty());
        // Moving a bookmark onto the bar keeps it once.
        loaded.AddBookmark({"https://good.example/", "Ignored"}, false);
        CHECK(loaded.bookmarks.Count() == 1);
        loaded.bookmarks.Move(loaded.bookmarks.FindURL("https://good.example/")->id, kOtherBookmarksFolder, 0);
        loaded.AddBookmark({"https://good.example/", "Ignored"}, true);
        CHECK(loaded.bookmarks.OnBar("https://good.example/") && loaded.bookmarks.Count() == 1
            && loaded.bookmarks.FindURL("https://good.example/")->title == "Good");
    }

    // Importing: into "Imported from …", without duplicates or empty folders.
    {
        Bookmarks marks;
        marks.Add({"https://known.example/", "Known"}, kBookmarksBarFolder);
        BookmarkNode imported{0, "", "", true, 0, {
            {0, "Toolbar", "", true, 0, {{0, "Known again", "https://known.example/", false, 0, {}},
                {0, "New", " https://new.example/ ", false, 5, {}}}},
            {0, "Only duplicates", "", true, 0, {{0, "Known", "https://known.example/", false, 0, {}}}},
            {0, "Smart", "place:sort=8", false, 0, {}},
            {0, "", "https://untitled.example/", false, 0, {}},
            {0, "Twice", "https://new.example/", false, 0, {}}}};
        auto result = marks.Import(imported, "Imported from Firefox", 99);
        CHECK(result.added == 2 && result.duplicates == 3 && result.unsupported == 1 && result.folder);
        const auto* folder = Child(marks.Other(), "Imported from Firefox");
        CHECK(folder && folder->id == result.folder && Titles(*folder) == "Toolbar/,https://untitled.example/");
        CHECK(Child(*folder, "Toolbar")->children.size() == 1 && Child(*folder, "Toolbar")->children[0].url == "https://new.example/");
        CHECK(Child(*folder, "Toolbar")->children[0].added == 5 && folder->children[1].added == 99);
        // Importing again finds everything there; nothing new, nothing removed.
        result = marks.Import(imported, "Imported from Firefox");
        CHECK(result.added == 0 && result.folder == 0 && Child(marks.Other(), "Imported from Firefox"));
        BookmarkNode more{0, "", "", true, 0, {{0, "Toolbar", "", true, 0, {{0, "More", "https://more.example/", false, 0, {}}}}}};
        result = marks.Import(more, "Imported from Firefox");
        CHECK(result.added == 1 && Child(*Child(marks.Other(), "Imported from Firefox"), "Toolbar")->children.size() == 2);
        CHECK(marks.Other().children.size() == 1);
        // Nothing to add makes no folder.
        CHECK(marks.Import(imported, "Imported from Chrome").added == 0 && !Child(marks.Other(), "Imported from Chrome"));
    }

    // Netscape bookmarks HTML, as Firefox exports it.
    const std::string firefoxHTML = R"(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<!-- This is an automatically generated file.
     It will be read and overwritten.
     DO NOT EDIT! -->
<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'none'; img-src data: *; object-src 'none'"></meta>
<TITLE>Bookmarks</TITLE>
<H1>Bookmarks Menu</H1>

<DL><p>
    <DT><A HREF="https://www.haiku-os.org/" ADD_DATE="1696000000" LAST_MODIFIED="1696000001" ICON_URI="https://www.haiku-os.org/favicon.ico" ICON="data:image/png;base64,AAAA">Haiku Project</A>
    <DT><H3 ADD_DATE="1696000002" LAST_MODIFIED="1696000003" PERSONAL_TOOLBAR_FOLDER="true">Bookmarks Toolbar</H3>
    <DL><p>
        <DT><A HREF="https://news.ycombinator.com/" ADD_DATE="1696000004">Hacker News</A>
        <DT><H3 ADD_DATE="1696000005">Rust &amp; C++</H3>
        <DL><p>
            <DT><A HREF="https://doc.rust-lang.org/book/" ADD_DATE="1696000006" TAGS="rust">The Rust Book &#8212; &lt;online&gt;</A>
            <DD>A description that is not a title
            <HR>
            <DT><A HREF='https://en.cppreference.com/' add_date=1696000007>cppreference</a>
        </DL><p>
        <DT><H3>Empty</H3>
        <DL><p>
        </DL><p>
    </DL><p>
    <DT><A HREF="place:sort=8&amp;maxResults=10">Recently Bookmarked</A>
    <DT><H3 ADD_DATE="1696000008" UNFILED_BOOKMARKS_FOLDER="true">Other Bookmarks</H3>
    <DL><p>
        <DT><A HREF="https://example.com/?a=1&amp;b=2">Example</A>
    </DL><p>
</DL>
)";
    {
        BookmarkNode root;
        std::string error;
        CHECK(ParseNetscapeBookmarks(firefoxHTML, root, error) && error.empty());
        CHECK(NetscapeBookmarksSource(firefoxHTML) == "Firefox");
        CHECK(Titles(root) == "Haiku Project,Bookmarks Toolbar/,Recently Bookmarked,Other Bookmarks/");
        CHECK(root.children[0].url == "https://www.haiku-os.org/" && root.children[0].added == 1696000000);
        const auto* toolbar = Child(root, "Bookmarks Toolbar");
        CHECK(toolbar && Titles(*toolbar) == "Hacker News,Rust & C++/,Empty/");
        const auto* rust = Child(*toolbar, "Rust & C++");
        CHECK(rust && rust->added == 1696000005 && Titles(*rust) == "The Rust Book — <online>,cppreference");
        CHECK(rust->children[1].url == "https://en.cppreference.com/" && rust->children[1].added == 1696000007);
        CHECK(Child(root, "Other Bookmarks")->children[0].url == "https://example.com/?a=1&b=2");
        CHECK(root.children[2].url == "place:sort=8&maxResults=10");
        CHECK(!ParseNetscapeBookmarks("<html><body>Hello</body></html>", root, error) && !error.empty());
        // Unclosed lists and stray tags still give what they can.
        CHECK(ParseNetscapeBookmarks("<DL><DT><H3>F</H3><DL><DT><A HREF=\"https://x.example/\">X", root, error));
        CHECK(Titles(root) == "F/" && root.children[0].children.size() == 1 && root.children[0].children[0].title == "X");
    }

    // Chrome's (and Chromium's) Bookmarks file.
    const std::string chromeJSON = R"({
   "checksum": "0123",
   "roots": {
      "bookmark_bar": {"children": [
            {"date_added": "13340000000000000", "guid": "1", "id": "5", "name": "Summit", "type": "url",
             "url": "https://github.com/jmgasper/summit"},
            {"children": [{"name": "WebKit", "type": "url", "url": "https://webkit.org/"},
                          {"children": [], "name": "Nothing", "type": "folder"}],
             "date_added": "13340000000000001", "name": "Engines", "type": "folder"}],
         "name": "Bookmarks bar", "type": "folder"},
      "other": {"children": [{"name": "Other one", "type": "url", "url": "https://other.example/"}],
         "name": "Other bookmarks", "type": "folder"},
      "synced": {"children": [{"name": "Phone", "type": "url", "url": "https://phone.example/"}],
         "name": "Mobile bookmarks", "type": "folder"}
   },
   "version": 1
})";
    {
        BookmarkNode root;
        std::string error;
        CHECK(ParseChromeBookmarks(chromeJSON, root, error));
        CHECK(Titles(root) == "Bookmarks bar/,Other one,Mobile bookmarks/");
        const auto* bar = Child(root, "Bookmarks bar");
        CHECK(Titles(*bar) == "Summit,Engines/" && bar->children[0].url == "https://github.com/jmgasper/summit");
        // Microseconds since 1601 become seconds since 1970.
        CHECK(bar->children[0].added == 13340000000 - 11644473600);
        CHECK(Titles(*Child(*bar, "Engines")) == "WebKit,Nothing/");
        CHECK(!ParseChromeBookmarks("{\"version\":1}", root, error) && !ParseChromeBookmarks("not json", root, error));
    }

    // Export, and reading the export back.
    {
        Bookmarks marks;
        marks.Add({"https://bar.example/?q=\"x\"&y", "Bar <one>"}, kBookmarksBarFolder, kBookmarkEnd, 1700000000);
        const auto folder = marks.AddFolder(kBookmarksBarFolder, "Tools & Things");
        marks.Add({"https://tool.example/", "Tool"}, folder);
        marks.Add({"https://other.example/", "Other"}, kOtherBookmarksFolder);
        const auto sub = marks.AddFolder(kOtherBookmarksFolder, "Sub");
        marks.Add({"file:///boot/home/notes.html", "Notes"}, sub);
        const auto html = WriteNetscapeBookmarks(marks);
        CHECK(html.rfind("<!DOCTYPE NETSCAPE-Bookmark-file-1>", 0) == 0 && NetscapeBookmarksSource(html) == "Summit");
        CHECK(html.find("PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks Bar</H3>") != std::string::npos);
        CHECK(html.find("HREF=\"https://bar.example/?q=&quot;x&quot;&amp;y\" ADD_DATE=\"1700000000\">Bar &lt;one&gt;</A>") != std::string::npos);
        BookmarkNode root;
        std::string error;
        CHECK(ParseNetscapeBookmarks(html, root, error));
        CHECK(Titles(root) == "Bookmarks Bar/,Other,Sub/");
        CHECK(Titles(*Child(root, "Bookmarks Bar")) == "Bar <one>,Tools & Things/");
        CHECK(Child(root, "Bookmarks Bar")->children[0].url == "https://bar.example/?q=\"x\"&y");
        CHECK(Child(*Child(root, "Bookmarks Bar"), "Tools & Things")->children[0].url == "https://tool.example/");
        CHECK(Child(root, "Sub")->children[0].url == "file:///boot/home/notes.html");
        // Into a fresh profile it comes back in one folder, structure kept.
        Bookmarks fresh;
        auto result = fresh.Import(root, "Imported from Summit");
        CHECK(result.added == 4 && Titles(*Child(fresh.Other(), "Imported from Summit")) == "Bookmarks Bar/,Other,Sub/");
        // Into the same profile everything is a duplicate.
        CHECK(marks.Import(root, "Imported from Summit").added == 0 && marks.Import(root, "Imported from Summit").duplicates == 4);
    }

    // Files on disk: ReadImport recognises each kind.
    {
        std::string error;
        ImportedBookmarks imported;
        BookmarkImportOptions options;
        options.history = true;
        WriteText(directory / "bookmarks.html", firefoxHTML);
        CHECK(ReadImport(directory / "bookmarks.html", options, imported, error) && imported.source == "Firefox");
        CHECK(Titles(imported.bookmarks).find("Bookmarks Toolbar/") != std::string::npos);
        WriteText(directory / "generic.html", "<DL><DT><A HREF=\"https://g.example/\">G</A></DL>");
        CHECK(ReadImport(directory / "generic.html", options, imported, error) && imported.source == "generic.html");
        const auto chromeProfile = directory / "Default";
        std::filesystem::create_directories(chromeProfile);
        WriteText(chromeProfile / "Bookmarks", chromeJSON);
        CHECK(ReadImport(chromeProfile / "Bookmarks", options, imported, error) && imported.source == "Chrome"
            && imported.history.empty() && Titles(imported.bookmarks) == "Bookmarks bar/,Other one,Mobile bookmarks/");
        CHECK(!ReadImport(directory / "missing.html", options, imported, error) && !error.empty());

        // WebPositive: a folder of bookmark files, the address read by a callback
        // (on Haiku, from the META:url attribute).
        const auto webPositive = directory / "WebPositive" / "Bookmarks";
        std::filesystem::create_directories(webPositive / "Haiku" / "Docs");
        WriteText(webPositive / "zeta", "https://zeta.example/");
        WriteText(webPositive / "Alpha", "https://alpha.example/");
        WriteText(webPositive / "Haiku" / "Haiku Project", "https://www.haiku-os.org/");
        WriteText(webPositive / "Haiku" / "Docs" / "User Guide", "https://www.haiku-os.org/docs/userguide/");
        WriteText(webPositive / "Haiku" / "not a bookmark", "");
        WriteText(webPositive / ".hidden", "https://hidden.example/");
        options.readBookmarkFile = [](const std::filesystem::path& file, std::string& url, std::string& title) {
            std::ifstream in(file);
            std::getline(in, url);
            title.clear();
            return !url.empty();
        };
        CHECK(ReadImport(webPositive, options, imported, error) && imported.source == "WebPositive");
        CHECK(Titles(imported.bookmarks) == "Alpha,Haiku/,zeta");
        const auto* haiku = Child(imported.bookmarks, "Haiku");
        CHECK(haiku && Titles(*haiku) == "Docs/,Haiku Project" && Child(*haiku, "Docs")->children[0].title == "User Guide");
        CHECK(Child(*haiku, "Haiku Project")->url == "https://www.haiku-os.org/" && Child(*haiku, "Haiku Project")->added > 0);
        Bookmarks marks;
        auto result = marks.Import(imported.bookmarks, "Imported from " + imported.source);
        CHECK(result.added == 4 && Titles(*Child(marks.Other(), "Imported from WebPositive")) == "Alpha,Haiku/,zeta");
    }

    // Firefox's places.sqlite and Chrome's History, through the system's SQLite.
    std::string sqlite = "libsqlite3.so.0";
    if (const char* library = std::getenv("SUMMIT_TEST_SQLITE")) sqlite = library;
    const auto places = directory / "places.sqlite";
    if (MakeDatabase(sqlite, places, R"(
        CREATE TABLE moz_places (id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR, visit_count INTEGER DEFAULT 0,
            hidden INTEGER DEFAULT 0 NOT NULL, last_visit_date INTEGER);
        CREATE TABLE moz_bookmarks (id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER DEFAULT NULL, parent INTEGER,
            position INTEGER, title LONGVARCHAR, dateAdded INTEGER, guid TEXT);
        INSERT INTO moz_places VALUES (1, 'https://www.mozilla.org/', 'Mozilla', 3, 0, 1700000000000000),
            (2, 'https://addons.mozilla.org/', 'Add-ons', 1, 0, 1700000100000000),
            (3, 'https://hidden.example/', 'Hidden', 5, 1, 1700000200000000),
            (4, 'place:tag=x', NULL, 0, 0, NULL), (5, 'https://in.example/', 'In', 0, 0, NULL);
        INSERT INTO moz_bookmarks VALUES (1, 2, NULL, 0, 0, '', 0, 'root________'),
            (2, 2, NULL, 1, 0, 'menu', 0, 'menu________'), (3, 2, NULL, 1, 1, 'toolbar', 0, 'toolbar_____'),
            (4, 2, NULL, 1, 2, 'tags', 0, 'tags________'), (5, 2, NULL, 1, 3, 'unfiled', 0, 'unfiled_____'),
            (10, 1, 2, 3, 1, 'Add-ons', 1690000000000000, 'a'), (11, 1, 1, 3, 0, 'Mozilla', 1690000000000000, 'b'),
            (12, 2, NULL, 3, 2, 'Nested', 1690000000000000, 'c'), (13, 1, 5, 12, 0, 'In nested', 0, 'd'),
            (14, 3, NULL, 2, 0, NULL, 0, 'e'), (15, 1, 4, 2, 1, 'Tagged', 0, 'f'),
            (16, 2, NULL, 4, 0, 'x', 0, 'g'), (17, 1, 1, 16, 0, NULL, 0, 'h');
    )")) {
        std::string error;
        ImportedBookmarks imported;
        BookmarkImportOptions options;
        options.sqliteLibrary = sqlite;
        CHECK(ReadImport(places, options, imported, error) && imported.source == "Firefox");
        if (!error.empty()) std::cerr << error << "\n";
        CHECK(Titles(imported.bookmarks) == "Bookmarks Toolbar/,Tagged");
        const auto* toolbar = Child(imported.bookmarks, "Bookmarks Toolbar");
        CHECK(toolbar && Titles(*toolbar) == "Mozilla,Add-ons,Nested/" && toolbar->children[0].added == 1690000000);
        CHECK(Child(*toolbar, "Nested")->children[0].url == "https://in.example/");
        CHECK(imported.history.size() == 2 && imported.history[0].url == "https://addons.mozilla.org/"
            && imported.history[0].visited == 1700000100 && imported.history[1].visits == 3);
        Profile profile;
        profile.Visit({"https://www.mozilla.org/", "Mozilla"}, 1600000000);
        CHECK(profile.ImportHistory(imported.history) == 1 && profile.history.size() == 2);
        CHECK(profile.history[0].url == "https://addons.mozilla.org/" && profile.history[1].visited == 1700000000
            && profile.history[1].visits == 3);
        CHECK(profile.ImportHistory(imported.history) == 0);

        const auto chromeHistory = directory / "Default" / "History";
        CHECK(MakeDatabase(sqlite, chromeHistory, R"(
            CREATE TABLE urls (id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR, visit_count INTEGER DEFAULT 0,
                typed_count INTEGER DEFAULT 0, last_visit_time INTEGER NOT NULL, hidden INTEGER DEFAULT 0 NOT NULL);
            INSERT INTO urls VALUES (1, 'https://chromium.org/', 'Chromium', 4, 0, 13340000000000000, 0),
                (2, 'https://hidden.example/', 'Hidden', 1, 0, 13340000000000000, 1);
        )"));
        CHECK(ReadImport(directory / "Default" / "Bookmarks", options, imported, error) && imported.history.size() == 1
            && imported.history[0].url == "https://chromium.org/" && imported.history[0].visited == 13340000000 - 11644473600);
        CHECK(ReadImport(chromeHistory, options, imported, error) && imported.history.size() == 1 && imported.bookmarks.children.empty());
        options.sqliteLibrary = "/nonexistent/libsqlite3.so.0";
        CHECK(!ReadImport(places, options, imported, error) && error.find("SQLite") != std::string::npos);
    } else std::cerr << "SQLite (" << sqlite << ") is not available: places.sqlite tests skipped\n";

    // The Bookmarks page and its links.
    {
        Bookmarks marks;
        const auto bar = marks.Add({"https://x.example/", "X <b>"}, kBookmarksBarFolder);
        const auto folder = marks.AddFolder(kOtherBookmarksFolder, "Reading \"list\"");
        marks.Add({"https://y.example/", "Y"}, folder);
        const auto html = RenderBookmarksPage(marks, [](const std::string& url) {
            return url == "https://x.example/" ? std::string("data:image/png;base64,QUJD") : std::string("javascript:evil");
        }, "0123456789abcdef");
        CHECK(html.find("data-token=\"0123456789abcdef\"") != std::string::npos);
        CHECK(html.find("summit-bookmarks:import?token=0123456789abcdef\"") != std::string::npos);
        CHECK(html.find("<div class=\"item\" data-id=\"" + std::to_string(bar) + "\"><div class=\"row\" data-title=\"X &lt;b&gt;\" "
            "data-url=\"https://x.example/\">") != std::string::npos && html.find("data-action=\"edit\"") != std::string::npos);
        CHECK(html.find("<option value=\"" + std::to_string(folder) + "\">\xC2\xA0\xC2\xA0\xC2\xA0\xC2\xA0Reading &quot;list&quot;</option>")
            != std::string::npos);
        CHECK(html.find("Reading &quot;list&quot;") != std::string::npos && html.find("X &lt;b&gt;") != std::string::npos);
        CHECK(html.find("<div class=\"item folder\" data-id=\"" + std::to_string(folder) + "\"") != std::string::npos);
        CHECK(html.find("1 bookmark<") != std::string::npos && html.find("evil") == std::string::npos);
        CHECK(html.find(".i0{background-image:url(data:image/png;base64,QUJD)}") != std::string::npos);
        // A token that is not hex is reduced to what the links can carry.
        CHECK(RenderBookmarksPage(marks, nullptr, "\"><script>").find("data-token=\"c\"") != std::string::npos);
        CHECK(RenderBookmarksPage(Bookmarks(), nullptr, "ab").find("No bookmarks yet") != std::string::npos);
        auto link = ParseBookmarkPageLink("summit-bookmarks:move?token=ab12&id=7&folder=2&index=3");
        CHECK(link && link->action == "move" && link->token == "ab12" && link->id == 7 && link->folder == 2 && link->index == 3);
        link = ParseBookmarkPageLink("Summit-Bookmarks:delete?token=ab&id=9");
        CHECK(link && link->action == "delete" && link->id == 9 && link->index == kBookmarkEnd);
        CHECK(!ParseBookmarkPageLink("summit-bookmarks:edit?id=7"));
        CHECK(!ParseBookmarkPageLink("summit-bookmarks:edit?token=XYZ&id=7"));
        CHECK(!ParseBookmarkPageLink("summit-bookmarkz:edit?token=ab&id=7"));
        CHECK(!ParseBookmarkPageLink("summit-bookmarks:Edit!?token=ab"));
        link = ParseBookmarkPageLink("summit-bookmarks:edit?token=ab&id=-4&folder=x");
        CHECK(link && link->id == 0 && link->folder == 0);
        link = ParseBookmarkPageLink("summit-bookmarks:save?token=ab&id=4&title=Caf%C3%A9%20%26%20bar%3D&url=https%3A%2F%2Fx.example%2F%3Fa%3D1&folder=2");
        CHECK(link && link->hasTitle && link->title == "Caf\xC3\xA9 & bar=" && link->hasURL && link->url == "https://x.example/?a=1");
        CHECK(link->folder == 2 && link->id == 4);
        link = ParseBookmarkPageLink("summit-bookmarks:save?token=ab&id=4&title=100%");
        CHECK(link && link->title == "100%" && !link->hasURL);
    }

    std::filesystem::remove_all(directory);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
