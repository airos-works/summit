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
    for (const auto& child : folder.children)
        text += (text.empty() ? "" : ",") + (child.separator ? "--" : child.title + (child.folder ? "/" : ""));
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
        CHECK(!marks.Add({"data:text/html,bad", "J"}, kOtherBookmarksFolder) && !marks.Add({"https://x.example/", "X"}, 999));
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
        CHECK(!marks.Edit(c, "See", "https://see.example/") && !marks.Edit(c, "Bad", "data:text/html,x"));
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
            R"json({"bar":[{"title":"Bad","url":"data:text/html,bad"},{"title":"Good","url":"https://good.example/"},7,)json"
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
        // A folder empty in the file comes in (an export of an empty folder
        // imported back), and the import folder with it.
        BookmarkNode empty{0, "", "", true, 0, {{0, "Later", "", true, 0, {}},
            {0, "Only duplicates", "", true, 0, {{0, "Known", "https://known.example/", false, 0, {}}}}}};
        result = marks.Import(empty, "Imported from a file");
        CHECK(result.Changed() && result.added == 0 && result.foldersAdded == 2 && result.folder);
        const auto* file = Child(marks.Other(), "Imported from a file");
        CHECK(file && Titles(*file) == "Later/");
        result = marks.Import(empty, "Imported from a file");
        CHECK(!result.Changed() && result.folder == 0);
        // The same through an export and the HTML reader.
        Bookmarks exported;
        exported.AddFolder(kOtherBookmarksFolder, "Nothing yet");
        exported.Add({"https://kept.example/", "Kept"}, kOtherBookmarksFolder);
        BookmarkNode read;
        std::string readError;
        CHECK(ParseNetscapeBookmarks(WriteNetscapeBookmarks(exported), read, readError));
        Bookmarks round;
        round.Import(read, "Imported");
        CHECK(Titles(*Child(round.Other(), "Imported")) == "Nothing yet/,Kept");
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
        CHECK(rust && rust->added == 1696000005 && Titles(*rust) == "The Rust Book — <online>,--,cppreference");
        CHECK(rust->children[1].separator && rust->children[1].url.empty());
        CHECK(rust->children[0].metadata.tags == std::vector<std::string>{"rust"});
        CHECK(rust->children[0].metadata.description == "A description that is not a title");
        CHECK(rust->children[2].url == "https://en.cppreference.com/" && rust->children[2].added == 1696000007);
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
        CHECK(Titles(imported.bookmarks) == "Bookmarks Toolbar/,--,Tagged");
        CHECK(imported.bookmarks.children[1].separator);
        const auto* toolbar = Child(imported.bookmarks, "Bookmarks Toolbar");
        CHECK(toolbar && Titles(*toolbar) == "Mozilla,Add-ons,Nested/" && toolbar->children[0].added == 1690000000);
        CHECK(Child(*toolbar, "Nested")->children[0].url == "https://in.example/");
        CHECK(toolbar->children[0].metadata.tags == std::vector<std::string>{"x"});
        CHECK(imported.history.size() == 2 && imported.history[0].url == "https://addons.mozilla.org/"
            && imported.history[0].visited == 1700000100 && imported.history[1].visits == 3);
        Profile profile;
        profile.Visit({"https://www.mozilla.org/", "Mozilla"}, 1600000000);
        CHECK(profile.ImportHistory(imported.history) == 1 && profile.history.size() == 2);
        CHECK(profile.history[0].url == "https://addons.mozilla.org/" && profile.history[1].visited == 1700000000
            && profile.history[1].visits == 3);
        CHECK(profile.ImportHistory(imported.history) == 0);

        CHECK(MakeDatabase(sqlite, places, R"(
            CREATE TABLE moz_keywords (id INTEGER PRIMARY KEY, keyword TEXT, place_id INTEGER, post_data TEXT);
            INSERT INTO moz_keywords VALUES (1, ' MOZ ', 1, NULL),
                (2, 'addons', 2, 'q%3D%25s%26from%3Dbookmark'), (3, 'extensions', 2, 'search=%s');
        )"));
        CHECK(ReadImport(places, options, imported, error));
        toolbar = Child(imported.bookmarks, "Bookmarks Toolbar");
        CHECK(toolbar->children[0].metadata.keywords.size() == 1 && toolbar->children[0].metadata.keywords[0].name == "moz");
        CHECK(toolbar->children[1].metadata.keywords.size() == 2);
        CHECK(toolbar->children[1].metadata.keywords[0].postData == "q%3D%25s%26from%3Dbookmark");
        CHECK(toolbar->children[1].metadata.keywords[1].postData == "search=%s");
        Bookmarks shortcuts;
        shortcuts.Import(imported.bookmarks, "Firefox");
        CHECK(shortcuts.ResolveKeyword("MOZ")->url == "https://www.mozilla.org/");
        CHECK(shortcuts.ResolveKeyword("addons café & tea")->postData == "q=caf%C3%A9%20%26%20tea&from=bookmark");
        CHECK(shortcuts.ResolveKeyword("extensions theme")->postData == "search=theme");

        // Old Firefox descriptions are optional item annotations. Tags are
        // URL associations under the tags root, not ordinary imported folders.
        CHECK(MakeDatabase(sqlite, places, R"(
            CREATE TABLE moz_anno_attributes (id INTEGER PRIMARY KEY, name TEXT);
            CREATE TABLE moz_items_annos (id INTEGER PRIMARY KEY, item_id INTEGER, anno_attribute_id INTEGER, content TEXT);
            INSERT INTO moz_anno_attributes VALUES (1, 'bookmarkProperties/description'), (2, 'unrelated'), (3, 'URIProperties/characterSet');
            CREATE TABLE moz_annos (id INTEGER PRIMARY KEY, place_id INTEGER, anno_attribute_id INTEGER, content TEXT);
            INSERT INTO moz_annos VALUES (1, 2, 3, 'ISO-8859-1');
            INSERT INTO moz_items_annos VALUES (1, 11, 1, 'Mozilla notes'), (2, 12, 1, 'Folder notes'), (3, 13, 2, 'Ignored');
            INSERT INTO moz_bookmarks VALUES (18, 2, NULL, 4, 1, 'tag, comma', 0, 'i'),
                (19, 1, 1, 18, 0, NULL, 0, 'j'), (20, 1, 5, 18, 1, NULL, 0, 'k');
        )"));
        CHECK(ReadImport(places, options, imported, error));
        toolbar = Child(imported.bookmarks, "Bookmarks Toolbar");
        CHECK(toolbar->children[0].metadata.description == "Mozilla notes");
        CHECK(toolbar->children[1].metadata.keywords[0].charset == "ISO-8859-1");
        CHECK((toolbar->children[0].metadata.tags == std::vector<std::string>{"x", "tag, comma"}));
        CHECK(Child(*toolbar, "Nested")->metadata.description == "Folder notes");
        CHECK(Child(*toolbar, "Nested")->children[0].metadata.description.empty());
        CHECK(Child(*toolbar, "Nested")->children[0].metadata.tags == std::vector<std::string>{"tag, comma"});
        CHECK(Titles(imported.bookmarks) == "Bookmarks Toolbar/,--,Tagged");

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
        // Haiku may resolve an absent absolute path by its library basename.
        // Use a missing name as well, so the installed SQLite cannot satisfy it.
        options.sqliteLibrary = "/nonexistent/summit-missing-sqlite-library.so";
        CHECK(!ReadImport(places, options, imported, error) && error.find("SQLite") != std::string::npos);
    } else std::cerr << "SQLite (" << sqlite << ") is not available: places.sqlite tests skipped\n";

    // Separators retain their position through import, save, move and export.
    // They never become pages, URLs, suggestions, or editable links.
    {
        BookmarkNode source;
        std::string error;
        CHECK(ParseNetscapeBookmarks(R"(<DL><HR NAME="&lt;named&gt;" ADD_DATE="1700000000">
            <DT><A HREF="https://before.example/">Before</A><HR><HR>
            <DT><H3>Nested</H3><DL><HR><DT><A HREF="https://nested.example/">In</A><HR></DL>
            <DT><A HREF="https://after.example/">After</A><HR></DL>)", source, error));
        CHECK(Titles(source) == "--,Before,--,--,Nested/,After,--" && CountBookmarks(source) == 3);
        CHECK(source.children[0].title == "<named>" && source.children[0].added == 1700000000);
        Profile profile;
        auto& marks = profile.bookmarks;
        auto imported = marks.Import(source, "Imported");
        CHECK(imported.Changed() && imported.added == 3 && imported.separatorsAdded == 6 && imported.foldersAdded == 2);
        CHECK(marks.Count() == 3 && marks.SeparatorCount() == 6 && marks.FolderCount() == 2);
        const auto folder = imported.folder;
        CHECK(folder && Titles(*marks.Find(folder)) == "--,Before,--,--,Nested/,After,--");
        CHECK(!marks.FindURL("") && !marks.Contains("") && !marks.OnBar(""));
        const auto first = marks.Find(folder)->children[0].id;
        CHECK(!marks.Edit(first, "Changed", "https://changed.example/"));
        imported = marks.Import(source, "Imported");
        CHECK(!imported.Changed() && imported.duplicates == 3 && imported.unsupported == 0 && marks.SeparatorCount() == 6);
        CHECK(Titles(*marks.Find(folder)) == "--,Before,--,--,Nested/,After,--");
        CHECK(marks.Move(first, kBookmarksBarFolder, 0) && marks.Bar().children[0].separator && !marks.OnBar(""));
        CHECK(marks.Count() == 3 && marks.SeparatorCount() == 6);
        const auto path = directory / "separators.json";
        CHECK(profile.Save(path, error));
        nlohmann::json saved;
        { std::ifstream in(path); in >> saved; }
        CHECK(saved["bookmarkTree"]["bar"][0]["separator"] == true && !saved["bookmarkTree"]["bar"][0].contains("url"));
        CHECK(saved["bookmarks"].size() == 3);
        auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.Count() == 3 && loaded.bookmarks.SeparatorCount() == 6);
        CHECK(loaded.bookmarks.Bar().children[0].separator && loaded.bookmarks.Bar().children[0].title == "<named>");
        CHECK(loaded.bookmarks.Bar().children[0].added == 1700000000);
        CHECK(Titles(loaded.bookmarks.Other().children[0]) == "Before,--,--,Nested/,After,--");
        const auto html = WriteNetscapeBookmarks(loaded.bookmarks);
        CHECK(html.find("<HR NAME=\"&lt;named&gt;\" ADD_DATE=\"1700000000\">") != std::string::npos);
        CHECK(ParseNetscapeBookmarks(html, source, error));
        Bookmarks round;
        imported = round.Import(source, "Round trip");
        CHECK(imported.added == 3 && imported.separatorsAdded == 6 && round.SeparatorCount() == 6);
        const auto& original = *Child(*round.Find(imported.folder), "Imported");
        CHECK(Titles(original) == "Before,--,--,Nested/,After,--");
        const auto page = RenderBookmarksPage(marks, nullptr, "ab");
        CHECK(page.find("class=\"item separator\" data-id=\"" + std::to_string(first) + "\"") != std::string::npos);
        CHECK(page.find("role=\"separator\" aria-label=\"&lt;named&gt;\"") != std::string::npos);
        CHECK(page.find("href=\"\"") == std::string::npos && page.find("data-url=\"\"") == std::string::npos);
        CHECK(marks.Remove(first) && marks.SeparatorCount() == 5 && marks.Count() == 3);
        const auto added = marks.AddSeparator(kBookmarksBarFolder, 0, 42);
        CHECK(added && marks.Find(added)->separator && marks.Find(added)->added == 42);
        CHECK(!marks.AddSeparator(999999) && !marks.AddSeparator(marks.FindURL("https://before.example/")->id));
    }

    // Separator-only imports count as changes, repeat without duplication,
    // and have their own bound without using the page or folder allowance.
    {
        BookmarkNode source;
        std::string error;
        CHECK(ParseNetscapeBookmarks("<HR><HR>", source, error));
        Bookmarks marks;
        auto result = marks.Import(source, "Separators");
        CHECK(result.Changed() && result.added == 0 && result.separatorsAdded == 2 && result.folder);
        CHECK(marks.Count() == 0 && !marks.Empty() && marks.SeparatorCount() == 2);
        CHECK(!marks.Import(source, "Separators").Changed());
        source.children.assign(kMaxBookmarkSeparators + 2, BookmarkNode{0, "", "", false, 0, {}, true});
        result = marks.Import(source, "Separators");
        CHECK(result.separatorsAdded == kMaxBookmarkSeparators - 2 && result.overLimit == 2);
        CHECK(marks.SeparatorCount() == kMaxBookmarkSeparators && !marks.AddSeparator(kOtherBookmarksFolder));
        CHECK(marks.Add({"https://limit.example/", "Still fits"}, kBookmarksBarFolder));
        Profile profile;
        profile.bookmarks = marks;
        const auto path = directory / "separator-limit.json";
        CHECK(profile.Save(path, error));
        nlohmann::json saved;
        { std::ifstream in(path); in >> saved; }
        saved["bookmarkTree"]["bar"].push_back({{"title", "Extra"}, {"separator", true}});
        saved["bookmarkTree"]["bar"].push_back({{"title", "Malformed"}, {"separator", "true"}});
        WriteText(path, saved.dump());
        const auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.SeparatorCount() == kMaxBookmarkSeparators && loaded.bookmarks.Count() == 1);
    }

    // At the folder limit, existing folders still merge normally. A new
    // import container cannot exceed that limit, even for separator-only input.
    {
        BookmarkNode bar, other;
        other.children.push_back({0, "Imported", "", true, 0,
            {{0, "Nested", "", true, 0, {{0, "", "", false, 0, {}, true}}}}});
        while (other.children.size() < kMaxBookmarkFolders - 1)
            other.children.push_back({0, "Folder " + std::to_string(other.children.size()), "", true, 0, {}});
        Bookmarks marks;
        marks.Assign(bar, other);
        BookmarkNode source = other.children.front();
        CHECK(marks.FolderCount() == kMaxBookmarkFolders);
        const auto result = marks.Import(source, "Imported");
        CHECK(!result.Changed() && result.overLimit == 0 && marks.SeparatorCount() == 1);
        CHECK(Titles(marks.Other().children.front()) == "Nested/");
        CHECK(marks.Import(source, "Another import").overLimit > 0 && marks.FolderCount() == kMaxBookmarkFolders);
        const auto id = marks.Add({"https://known.example/", "Keep my title"}, kBookmarksBarFolder);
        BookmarkNode metadata;
        metadata.children.push_back({0, "Imported title", "https://known.example/", false, 0, {}});
        metadata.children.back().metadata = {"Imported description", {"new tag"}};
        const auto updated = marks.Import(metadata, "No room for this folder");
        CHECK(updated.Changed() && updated.updated == 1 && updated.duplicates == 1 && updated.overLimit == 0);
        CHECK(updated.folder == 0 && updated.foldersAdded == 0 && marks.FolderCount() == kMaxBookmarkFolders);
        CHECK(marks.Find(id)->metadata == metadata.children.back().metadata && marks.Find(id)->title == "Keep my title");
        CHECK(!marks.Import(metadata, "No room for this folder").Changed());
    }

    // Keyword aliases retain their request templates through imports and edits.
    {
        BookmarkNode source;
        std::string error;
        CHECK(ParseNetscapeBookmarks(R"(<DL>
            <DT><A HREF="https://search.example/?q=%s&amp;raw=%S" SHORTCUTURL=" Find " TAGS="search">Find</A>
            <DT><A HREF="https://post.example/search" SHORTCUTURL="post" POST_DATA="q%3D%25s%26fixed%3Da%2520b">POST</A>
            <DT><A HREF="https://post.example/search" SHORTCUTURL="raw" POST_DATA="q=%S">Alias</A>
            <DT><A HREF="https://legacy.example/?q=%s" SHORTCUTURL="legacy" LAST_CHARSET="ISO-8859-1">Legacy</A>
            </DL>)", source, error));
        CHECK(source.children[0].metadata.keywords[0].name == "find");
        Profile profile;
        auto imported = profile.bookmarks.Import(source, "Imported");
        CHECK(imported.added == 3 && imported.duplicates == 1);
        const auto* post = profile.bookmarks.FindURL("https://post.example/search");
        CHECK(post->metadata.keywords.size() == 2);
        const auto aliases = post->metadata.keywords;
        CHECK(BookmarkKeywordsText(aliases) == "post, raw");
        const auto edited = EditBookmarkKeywords(aliases, "POST, added");
        CHECK(edited.size() == 2 && edited[0] == aliases[0] && edited[1].name == "added" && edited[1].postData == aliases[0].postData);
        CHECK(EditBookmarkKeywords(aliases, "renamed")[0].postData == aliases[0].postData);
        const auto resolved = profile.bookmarks.ResolveKeyword("FiNd café & tea");
        CHECK(resolved && resolved->error.empty() && !resolved->post);
        CHECK(resolved->url == "https://search.example/?q=caf%C3%A9%20%26%20tea&raw=café & tea");
        CHECK(profile.bookmarks.ResolveKeyword("post café & tea")->postData == "q=caf%C3%A9%20%26%20tea&fixed=a%20b");
        CHECK(profile.bookmarks.ResolveKeyword("raw a+b&c")->postData == "q=a+b&c");
        CHECK(profile.bookmarks.ResolveKeyword("legacy café")->url == "https://legacy.example/?q=caf%E9");
        CHECK(!profile.bookmarks.ResolveKeyword("missing text"));
        CHECK(!profile.bookmarks.ResolveKeyword("https://post.example/search"));
        CHECK(!profile.bookmarks.ResolveKeyword("post\ntext"));
        CHECK(profile.bookmarks.ResolveKeyword("post")->postData == "q=&fixed=a%20b");
        CHECK(profile.bookmarks.ResolveKeyword("find %S")->url == "https://search.example/?q=%25S&raw=%S");
        CHECK(profile.bookmarks.ResolveKeyword("post a!'()*")->postData == "q=a!'()*&fixed=a%20b");
        const auto path = directory / "keywords.json";
        CHECK(profile.Save(path, error));
        auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.FindURL(post->url)->metadata.keywords == aliases);
        CHECK(loaded.bookmarks.ResolveKeyword("post two words")->postData == "q=two%20words&fixed=a%20b");
        const auto html = WriteNetscapeBookmarks(profile.bookmarks);
        CHECK(html.find("SHORTCUTURL=\"post\" POST_DATA=\"q%3D%25s%26fixed%3Da%2520b\"") != std::string::npos);
        CHECK(html.find("LAST_CHARSET=\"ISO-8859-1\"") != std::string::npos && html.find("SUMMIT_KEYWORDS=") != std::string::npos);
        BookmarkNode parsed;
        CHECK(ParseNetscapeBookmarks(html, parsed, error));
        Bookmarks round;
        round.Import(parsed, "Round trip");
        CHECK(round.FindURL(post->url)->metadata.keywords == aliases);
        CHECK(round.ResolveKeyword("raw exact")->postData == "q=exact");
        CHECK(round.ResolveKeyword("legacy café")->url == "https://legacy.example/?q=caf%E9");
        CHECK(!profile.bookmarks.Import(source, "Imported").Changed());
        source.children[1].metadata.keywords.push_back({"new", "different=%s", {}});
        const auto enriched = profile.bookmarks.Import(source, "Imported");
        CHECK(enriched.Changed() && enriched.updated == 1 && !enriched.added);
        CHECK(profile.bookmarks.ResolveKeyword("new text")->postData == "different=text");
        const auto page = RenderBookmarksPage(profile.bookmarks, nullptr, "ab");
        CHECK(page.find("data-keywords=\"post, raw, new\"") != std::string::npos);
        auto link = ParseBookmarkPageLink("summit-bookmarks:save?token=ab&id=3&keywords=post%2Cnew");
        CHECK(link && link->hasKeywords && link->keywords == "post,new");
    }

    // Fixed shortcuts, conflicts, legacy escapes, charset selection and bounds.
    {
        Bookmarks marks;
        const auto id = marks.Add({"https://fixed.example/", "Fixed"}, kOtherBookmarksFolder);
        auto set = [&](std::string url, std::string post = "", std::string charset = "") {
            marks.Edit(id, "Shortcut", url);
            BookmarkMetadata metadata;
            metadata.keywords = {{"key", post, charset}};
            marks.SetMetadata(id, metadata);
        };
        set("https://fixed.example/");
        CHECK(marks.ResolveKeyword("KEY")->url == "https://fixed.example/");
        CHECK(!marks.ResolveKeyword("key ignored"));
        set("https://fixed.example/?q=%s&mozcharset=ISO-8859-1");
        CHECK(marks.ResolveKeyword("key café")->url == "https://fixed.example/?q=caf%E9");
        set("https://fixed.example/?q=%s", "", "unknown-charset");
        CHECK(marks.ResolveKeyword("key café")->url == "https://fixed.example/?q=caf%C3%A9");
        set("https://fixed.example/", "query=%s&accent=%E9&face=%uD83D%uDE00&bad=%QZ");
        CHECK(marks.ResolveKeyword("key test")->postData == "query=test&accent=é&face=😀&bad=%QZ");
        set("https://fixed.example/", "constant=literal%20data");
        CHECK(marks.ResolveKeyword("key")->post && marks.ResolveKeyword("key")->postData == "constant=literal%20data");
        CHECK(!marks.ResolveKeyword("key ignored"));
        set("https://fixed.example/", "q=%s&nul=%00");
        CHECK(!marks.ResolveKeyword("key test")->error.empty());
        set("file:///boot/home/test", "q=%s");
        CHECK(!marks.ResolveKeyword("key test")->error.empty());
        set("https://fixed.example/?q=%s");
        const auto other = marks.Add({"https://other.example/", "Other"}, kBookmarksBarFolder);
        marks.SetMetadata(other, marks.Find(id)->metadata);
        CHECK(!marks.ResolveKeyword("key text")->error.empty());
        marks.SetMetadata(other, {});
        CHECK(marks.ResolveKeyword("key text")->error.empty());
        std::string huge = "https://fixed.example/?q=";
        for (int i = 0; i < 10000; ++i) huge += "%s";
        set(huge);
        CHECK(!marks.ResolveKeyword("key " + std::string(1000, 'x'))->error.empty());
        set("https://fixed.example/", std::string(10000, 'x') + "%s");
        CHECK(marks.ResolveKeyword("key " + std::string(60000, 'x'))->postData.size() == 70000);
        BookmarkMetadata metadata;
        metadata.keywords = {{" UPPER ", "x=%s", {}}, {"upper", "do-not-replace=%s", {}}, {"bad name", {}, {}},
            {std::string(kMaxBookmarkKeywordLength + 1, 'x'), {}, {}}, {"huge", std::string(kMaxBookmarkPostData + 1, 'x'), {}}};
        auto normalized = NormalizeBookmarkMetadata(metadata);
        CHECK(normalized.keywords.size() == 1 && normalized.keywords[0].name == "upper" && normalized.keywords[0].postData == "x=%s");
        metadata.keywords.clear();
        for (size_t i = 0; i <= kMaxBookmarkKeywords; ++i) metadata.keywords.push_back({"key" + std::to_string(i), {}, {}});
        CHECK(NormalizeBookmarkMetadata(metadata).keywords.size() == kMaxBookmarkKeywords);
        CHECK(EditBookmarkKeywords({}, std::string(kMaxBookmarkKeywordLength + 1, 'x')).empty());
    }

    // Metadata survives parsing, merge, editing, profile save and HTML export.
    {
        BookmarkNode source;
        std::string error;
        CHECK(ParseNetscapeBookmarks(R"(<DL>
            <DT><H3 TAGS="folder, research">Notes</H3><DD>Folder <em>description</em>
            <DL><DT><A HREF="https://metadata.example/" TAGS=" one, two,one,,three ">Imported title</A>
            <DD>Read <a href="https://inline.example/">this link</a> &amp; more
            <DT><A HREF="https://second.example/">Second</A><DD>Second description</DD>
            </DL><DT><A HREF="https://outside.example/">Outside</A></DL>)", source, error));
        CHECK(CountBookmarks(source) == 3 && source.children.size() == 2);
        CHECK(source.children[0].metadata.description == "Folder description");
        CHECK((source.children[0].metadata.tags == std::vector<std::string>{"folder", "research"}));
        const auto metadata = source.children[0].children[0].metadata;
        CHECK(metadata.description == "Read this link & more");
        CHECK((metadata.tags == std::vector<std::string>{"one", "two", "three"}));
        CHECK(source.children[0].children[1].metadata.description == "Second description");
        CHECK(source.children[1].metadata.Empty());
        BookmarkNode describedRoot;
        CHECK(ParseNetscapeBookmarks("<DL><DT><H3 PERSONAL_TOOLBAR_FOLDER='true'>Toolbar</H3>"
            "<DD>Keep these notes<DL></DL></DL>", describedRoot, error));
        CHECK(describedRoot.children.size() == 1 && describedRoot.children[0].metadata.description == "Keep these notes");

        Profile profile;
        auto result = profile.bookmarks.Import(source, "Imported");
        CHECK(result.added == 3 && result.Changed());
        const auto id = profile.bookmarks.FindURL("https://metadata.example/")->id;
        CHECK(profile.bookmarks.Find(id)->metadata == metadata);
        BookmarkMetadata edited{"<notes> & \"quotes\"\n  next\tline\n", {"comma, tag", "quote\"tag", "Café"}};
        CHECK(profile.bookmarks.SetMetadata(id, edited));
        CHECK(!profile.bookmarks.SetMetadata(id, edited));
        CHECK(ParseBookmarkTags(BookmarkTagsText(edited.tags)) == edited.tags);
        CHECK((ParseBookmarkTags("one, \"comma, tag\",\"quote\"\"tag\", one,, ") == std::vector<std::string>{"one", "comma, tag", "quote\"tag"}));
        const auto path = directory / "metadata.json";
        CHECK(profile.Save(path, error));
        auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.FindURL("https://metadata.example/")->metadata == edited);
        const auto html = WriteNetscapeBookmarks(loaded.bookmarks);
        CHECK(html.find("<DD>&lt;notes&gt;") != std::string::npos && html.find("SUMMIT_TAGS=") != std::string::npos);
        CHECK(ParseNetscapeBookmarks(html, source, error));
        Bookmarks round;
        CHECK(round.Import(source, "Round trip").added == 3);
        CHECK(round.FindURL("https://metadata.example/")->metadata == edited);
        CHECK(Child(round.Other().children[0].children[0], "Notes")->metadata.description == "Folder description");
        const auto page = RenderBookmarksPage(profile.bookmarks, nullptr, "ab");
        CHECK(page.find("data-description=\"&lt;notes&gt; &amp; &quot;quotes&quot;") != std::string::npos);
        CHECK(page.find("<span class=\"metadata\">") != std::string::npos && page.find("<notes>") == std::string::npos);
        CHECK(page.find("row.dataset.tags, row.dataset.description") != std::string::npos);
        auto link = ParseBookmarkPageLink("summit-bookmarks:save?token=ab&id=4&description=line%201%0Aline%202&tags=one%2C%20%22comma%2C%20tag%22");
        CHECK(link && link->hasDescription && link->hasTags && link->description == "line 1\nline 2");
        CHECK((ParseBookmarkTags(link->tags) == std::vector<std::string>{"one", "comma, tag"}));
        CHECK(profile.bookmarks.SetMetadata(id, {}) && profile.bookmarks.Find(id)->metadata.Empty());
        CHECK(profile.Save(path, error));
        CHECK(Profile::Load(path, error).bookmarks.FindURL("https://metadata.example/")->metadata.Empty());
        nlohmann::json saved;
        { std::ifstream input(path); input >> saved; }
        auto& item = saved["bookmarkTree"]["other"][0]["children"][0]["children"][0];
        item["description"] = 42;
        item["tags"] = {false, "valid", nullptr};
        WriteText(path, saved.dump());
        loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.FindURL("https://metadata.example/")->metadata.description.empty());
        CHECK(loaded.bookmarks.FindURL("https://metadata.example/")->metadata.tags == std::vector<std::string>{"valid"});
        CHECK(!profile.bookmarks.SetMetadata(kBookmarksBarFolder, edited));
        CHECK(!profile.bookmarks.SetMetadata(profile.bookmarks.AddSeparator(kBookmarksBarFolder), edited));
    }

    // Reimporting metadata enriches existing bookmarks without replacing their
    // title, folder, or nonempty description, and without creating an empty import.
    {
        Bookmarks marks;
        const auto id = marks.Add({"https://known.example/", "My title"}, kBookmarksBarFolder);
        marks.SetMetadata(id, {"My description", {"mine"}});
        BookmarkNode source;
        std::string error;
        CHECK(ParseNetscapeBookmarks("<DL><DT><A HREF='https://known.example/' TAGS='new,mine'>Different</A>"
            "<DD>Different description</DL>", source, error));
        auto result = marks.Import(source, "Imported");
        CHECK(result.Changed() && result.added == 0 && result.updated == 1 && result.duplicates == 1 && result.folder == 0);
        CHECK(marks.Other().children.empty() && marks.Find(id)->title == "My title");
        CHECK(marks.Find(id)->metadata.description == "My description");
        CHECK((marks.Find(id)->metadata.tags == std::vector<std::string>{"mine", "new"}));
        CHECK(!marks.Import(source, "Imported").Changed());
        marks.SetMetadata(id, {});
        CHECK(marks.Import(source, "Imported").updated == 1);
        CHECK(marks.Find(id)->metadata.description == "Different description");
        // Duplicate records inside the same import also contribute metadata.
        source.children.push_back(source.children[0]);
        source.children.back().metadata.tags = {"extra"};
        Bookmarks fresh;
        result = fresh.Import(source, "Imported");
        CHECK(result.added == 1 && result.duplicates == 1);
        CHECK((fresh.FindURL("https://known.example/")->metadata.tags == std::vector<std::string>{"new", "mine", "extra"}));
        BookmarkNode folder{0, "", "", true, 0, {{0, "Described folder", "", true, 0, source.children}}};
        folder.children[0].metadata.description = "Keep even when its links are already bookmarked";
        result = marks.Import(folder, "Imported");
        CHECK(result.foldersAdded == 2 && marks.Other().children[0].children[0].children.empty());
        CHECK(!marks.Other().children[0].children[0].metadata.Empty());
        CHECK(!marks.Import(folder, "Imported").Changed());
    }

    // Metadata bounds preserve UTF-8 boundaries, remove embedded NULs, and
    // discard duplicate/empty tags without affecting ordinary bookmark data.
    {
        BookmarkMetadata metadata;
        metadata.description = std::string(kMaxBookmarkDescription - 1, 'a') + "é";
        metadata.tags = {"", "  ", " A ", "A", std::string("nul\0tag", 7)};
        auto normalized = NormalizeBookmarkMetadata(metadata);
        CHECK(normalized.description.size() == kMaxBookmarkDescription - 1);
        CHECK((normalized.tags == std::vector<std::string>{"A", "nultag"}));
        metadata.tags.clear();
        for (size_t i = 0; i < kMaxBookmarkTags + 2; ++i)
            metadata.tags.push_back(std::to_string(i) + std::string(kMaxBookmarkTagLength, 'x'));
        normalized = NormalizeBookmarkMetadata(std::move(metadata));
        CHECK(normalized.tags.size() == kMaxBookmarkTags && normalized.tags[0].size() == kMaxBookmarkTagLength);
    }

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
            "data-url=\"https://x.example/\"") != std::string::npos && html.find("data-action=\"edit\"") != std::string::npos);
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

    // Bookmarklets remain source text through import, both editors' address
    // normalization, profile persistence and export. The manager never runs
    // that text as one of its own links.
    {
        const std::string script = "JaVaScRiPt:void(document.title='Café & <test> %25')";
        CHECK(IsBookmarkletURL(script) && IsBookmarkableURL(script));
        CHECK(IsBookmarkletURL("javascript:") && !IsBookmarkletURL("javascriptx:alert(1)"));
        CHECK(!IsBookmarkletURL(std::string("javascript:x\0y", 14)));
        CHECK(!IsBookmarkableURL("javascript:" + std::string(65536, 'x')));
        CHECK(NormalizeBookmarkAddress("  " + script + "  ") == script);
        CHECK(NormalizeBookmarkAddress("example.com") == "https://example.com");
        CHECK(NormalizeBookmarkAddress("FILE:///boot/home/a.html") == "FILE:///boot/home/a.html");
        Profile profile;
        auto& marks = profile.bookmarks;
        const auto id = marks.Add({script, "Mark <page>"}, kBookmarksBarFolder);
        CHECK(id && marks.Count() == 1 && marks.Pages()[0].url == script);
        CHECK(marks.SetMetadata(id, {"A bookmarklet", {"tools"}, {{"mark", "", ""}}}));
        CHECK(marks.ResolveKeyword("mark") && marks.ResolveKeyword("mark")->url == script && marks.ResolveKeyword("mark")->error.empty());
        CHECK(!marks.ResolveKeyword(script));
        const auto html = WriteNetscapeBookmarks(marks);
        BookmarkNode imported;
        std::string error;
        CHECK(ParseNetscapeBookmarks(html, imported, error));
        Bookmarks fresh;
        const auto importedResult = fresh.Import(imported, "Imported");
        CHECK(importedResult.added == 1 && importedResult.unsupported == 0);
        CHECK(fresh.FindURL(script) && fresh.FindURL(script)->metadata == marks.Find(id)->metadata);
        CHECK(fresh.Import(imported, "Imported").duplicates == 1 && fresh.Count() == 1);
        const auto path = directory / "bookmarklets.json";
        CHECK(profile.Save(path, error));
        const auto loaded = Profile::Load(path, error);
        CHECK(error.empty() && loaded.bookmarks.FindURL(script) && loaded.bookmarks.OnBar(script));
        CHECK(loaded.bookmarks.FindURL(script)->metadata == marks.Find(id)->metadata);
        const auto manager = RenderBookmarksPage(marks, nullptr, "ab12");
        CHECK(manager.find("href=\"JaVaScRiPt:") == std::string::npos && manager.find("href=\"javascript:") == std::string::npos);
        CHECK(manager.find("Bookmarklet — run on a webpage") != std::string::npos);
        CHECK(manager.find("Mark &lt;page&gt;") != std::string::npos && manager.find("data-url=\"JaVaScRiPt:") != std::string::npos);
        CHECK(marks.Edit(id, "Changed", "javascript:void(window.changed=1)") && marks.Count() == 1);
        CHECK(marks.Move(id, kOtherBookmarksFolder, kBookmarkEnd) && !marks.OnBar("javascript:void(window.changed=1)"));
        const auto parameter = marks.Add({"javascript:void(window.term='%s')", "Parameterized"}, kBookmarksBarFolder);
        CHECK(marks.SetMetadata(parameter, {{}, {}, {{"term", "", ""}}}));
        const auto bound = marks.ResolveKeyword("term café & tea");
        CHECK(bound && bound->error.empty() && bound->url == "javascript:void(window.term='caf%C3%A9%20%26%20tea')");
        CHECK(marks.SetMetadata(parameter, {{}, {}, {{"term", "q=%s", ""}}}));
        CHECK(!marks.ResolveKeyword("term x")->error.empty());
        BookmarkNode chrome;
        const auto chromeSource = nlohmann::json{{"roots", {{"bookmark_bar", {{"type", "folder"}, {"children", nlohmann::json::array({{{"type", "url"}, {"name", "Chrome script"}, {"url", script}}})}}}}}};
        CHECK(ParseChromeBookmarks(chromeSource.dump(), chrome, error));
        Bookmarks chromeMarks;
        CHECK(chromeMarks.Import(chrome, "Chrome").added == 1 && chromeMarks.Contains(script));

        const auto db = directory / "bookmarklets.sqlite";
        if (MakeDatabase(sqlite, db, R"(
            CREATE TABLE moz_places (id INTEGER PRIMARY KEY, url TEXT, title TEXT);
            CREATE TABLE moz_bookmarks (id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER, parent INTEGER, position INTEGER, title TEXT, dateAdded INTEGER, guid TEXT);
            INSERT INTO moz_places VALUES(1, 'javascript:void(window.firefox=1)', 'Firefox script');
            INSERT INTO moz_bookmarks VALUES(1,2,NULL,0,0,'',0,'root________'), (2,2,NULL,1,0,'Menu',0,'menu________'), (3,1,1,2,0,'Firefox script',1700000000000000,'bookmark____');
            CREATE TABLE moz_keywords (id INTEGER PRIMARY KEY, keyword TEXT, place_id INTEGER, post_data TEXT);
            INSERT INTO moz_keywords VALUES(1,'fx',1,NULL);
        )")) {
            BookmarkImportOptions options;
            options.sqliteLibrary = sqlite;
            options.history = false;
            ImportedBookmarks firefox;
            CHECK(ReadImport(db, options, firefox, error));
            Bookmarks firefoxMarks;
            CHECK(firefoxMarks.Import(firefox.bookmarks, "Firefox").added == 1);
            const auto fx = firefoxMarks.ResolveKeyword("fx");
            CHECK(fx && fx->url == "javascript:void(window.firefox=1)" && fx->error.empty());
        }
    }

    std::filesystem::remove_all(directory);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
