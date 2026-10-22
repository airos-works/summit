#include "core/Bookmarks.h"
#include "core/BookmarkQueries.h"
#include "core/Profile.h"
#include "core/HistoryVisits.h"
#include <dlfcn.h>
#include "ui/BookmarkImport.h"
#include "ui/BookmarkEditor.h"
#include "ui/Chrome.h"
#include "ui/Messages.h"
#include "ui/SharedProfile.h"
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <Menu.h>
#include <MenuItem.h>
#include <SeparatorItem.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <algorithm>

using namespace summit;
namespace {
constexpr uint32 kExercise = 'bkex', kFence = 'bkfn', kDone = 'bkdn';
int checks = 0, failures = 0;
void Check(bool pass, const char* name)
{
    ++checks;
    if (!pass) ++failures;
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}
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

std::shared_ptr<BookmarkButton> Snapshot()
{
    auto root = std::make_shared<BookmarkButton>();
    root->folder = true;
    BookmarkButton separator;
    separator.id = 3;
    separator.separator = true;
    BookmarkButton page;
    page.id = 4;
    page.title = "Example";
    page.url = "https://example.com/";
    root->children = {separator, page, separator};
    return root;
}
class TestWindow final : public BWindow {
public:
    TestWindow() : BWindow(BRect(20, 20, 420, 70), "Bookmark tests", B_TITLED_WINDOW, B_NOT_RESIZABLE)
    {
        fBar = new BookmarksBar;
        AddChild(fBar);
        fBar->ResizeTo(400, 27);
        fBar->SetBookmarks(Snapshot());
        Run(); // Keep the window hidden; it only receives this test's messages.
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kExercise) {
            fBar->MouseDown(BPoint(10, 10)); // Separator: never opens an address.
            fBar->MouseUp(BPoint(10, 10));
            fBar->MouseDown(BPoint(30, 10)); // The adjacent bookmark still opens.
            fBar->MouseUp(BPoint(30, 10));
            PostMessage(kFence);
        } else if (message->what == kOpenBookmark) {
            ++fOpened;
            fURL = message->GetString("url", "");
        } else if (message->what == kFence) {
            BMessage done(kDone);
            done.AddInt32("opened", fOpened);
            done.AddString("url", fURL.c_str());
            be_app_messenger.SendMessage(&done);
            Quit();
        } else BWindow::MessageReceived(message);
    }
private:
    BookmarksBar* fBar;
    int32 fOpened = 0;
    std::string fURL;
};
class Tests final : public BApplication {
public:
    explicit Tests(std::filesystem::path directory)
        : BApplication("application/x-vnd.Summit-BookmarkTests"), fDirectory(std::move(directory)) { }
    void ReadyToRun() override
    {
        std::filesystem::create_directories(fDirectory);
        const auto profilePath = fDirectory / "profile.json";
        Check(!std::filesystem::exists(profilePath), "isolated profile starts empty");
        if (std::filesystem::exists(profilePath)) { PostMessage(B_QUIT_REQUESTED); return; }
        fShared = std::make_unique<SharedProfile>(profilePath);
        fShared->AddListener(BMessenger(this));
        std::string error;
        Check(fShared->Save(error), "initial profile saved before import");
        const auto emptyFile = fDirectory / "empty.html";
        { std::ofstream file(emptyFile); file << "<DL><DT><H3>Empty folder</H3><DL></DL></DL>"; }
        auto result = ImportBookmarksFrom(*fShared, emptyFile, false);
        Check(result.ok && result.bookmarks == 0 && result.folder, "empty-folder import reports a change");
        Check(result.message.find("folder") != std::string::npos && result.message.find("no new") == std::string::npos,
            "empty-folder result describes the imported folders");
        Check(fShared->Save(error), "folder-only import can be saved");
        auto loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.bookmarks.FolderCount() == 2 && loaded.bookmarks.Count() == 0,
            "folder-only import survives reopening the saved profile");
        result = ImportBookmarksFrom(*fShared, emptyFile, false);
        Check(result.ok && !result.folder, "repeated empty-folder import has no change");
        const auto separatorsFile = fDirectory / "separators.html";
        { std::ofstream file(separatorsFile); file << "<DL><HR><HR></DL>"; }
        result = ImportBookmarksFrom(*fShared, separatorsFile, false);
        Check(result.ok && result.bookmarks == 0 && result.folder && result.message.find("2 separators") != std::string::npos,
            "separator-only import is reported");
        Check(fShared->Save(error), "separator-only import can be saved");
        loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.bookmarks.SeparatorCount() == 2 && loaded.bookmarks.Count() == 0,
            "separator-only import survives reopening the saved profile");
        result = ImportBookmarksFrom(*fShared, separatorsFile, false);
        Check(result.ok && !result.folder, "repeated separator import has no change");

        const auto metadataFile = fDirectory / "metadata.html";
        { std::ofstream file(metadataFile); file << "<DL><DT><A HREF='https://metadata.example/' TAGS='first'>Metadata</A></DL>"; }
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.bookmarks == 1, "bookmark with metadata imported");
        Check(fShared->Save(error), "initial metadata import saved");
        { std::ofstream file(metadataFile); file << "<DL><DT><A HREF='https://metadata.example/' TAGS='first,second'>Other title</A>"
            "<DD>Imported description</DL>"; }
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.bookmarks == 0 && result.message.find("Updated bookmark metadata") != std::string::npos,
            "metadata-only import reports an update");
        Check(fShared->Save(error), "metadata-only import saved");
        loaded = Profile::Load(profilePath, error);
        const auto* updated = loaded.bookmarks.FindURL("https://metadata.example/");
        Check(updated && updated->title == "Metadata" && updated->metadata.description == "Imported description"
            && updated->metadata.tags == std::vector<std::string>{"first", "second"},
            "metadata-only import survives reopening without replacing the title");
        result = ImportBookmarksFrom(*fShared, metadataFile, false);
        Check(result.ok && result.message.find("Updated") == std::string::npos, "identical metadata reimport changes nothing");

        const auto scriptFile = fDirectory / "bookmarklet.html";
        { std::ofstream file(scriptFile); file << "<DL><DT><A HREF='javascript:void(window.saved=1)'>Script</A></DL>"; }
        result = ImportBookmarksFrom(*fShared, scriptFile, false);
        Check(result.ok && result.bookmarks == 1 && result.message.find("could not be kept") == std::string::npos,
            "native import accepts a bookmarklet without an unsupported-address warning");
        Check(fShared->Save(error), "bookmarklet import saved");
        loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.bookmarks.Contains("javascript:void(window.saved=1)"), "native bookmarklet import survives reopening");

        BookmarkEditor::Fields fields;
        fields.title = "Editor test";
        fields.url = "JaVaScRiPt:void(document.title='Edited')";
        fields.metadata = {"Original\nnotes", {"one", "comma, tag"}};
        fields.metadata.keywords = {{"search", "q=%s", {}}};
        fields.folders = {{kOtherBookmarksFolder, 0, "Other Bookmarks"}};
        auto* editor = new BookmarkEditor(fields, BMessenger(this), BRect(0, 0, 800, 600));
        fEditor = BMessenger(editor);
        auto* tags = dynamic_cast<BTextControl*>(editor->FindView("tags"));
        auto* description = dynamic_cast<BTextView*>(editor->FindView("description"));
        auto* keywords = dynamic_cast<BTextControl*>(editor->FindView("keywords"));
        auto* save = dynamic_cast<BButton*>(editor->FindView("save"));
        Check(tags && description && save, "native editor exposes tags and a multiline description");
        Check(keywords && std::string(keywords->Text()) == "search", "native editor shows imported shortcut names");
        if (tags && description && save) {
            Check(ParseBookmarkTags(tags->Text()) == fields.metadata.tags && description->Text() == fields.metadata.description,
                "native editor shows imported metadata without losing commas or newlines");
            tags->SetText("one, \"comma, tag\", \"quote\"\"tag\"");
            description->SetText("Edited\nsecond line");
            if (keywords) keywords->SetText("search, alias");
            save->Invoke();
        } else editor->PostMessage(B_QUIT_REQUESTED);
        editor->Run();

        auto snapshot = Snapshot();
        BMenu menu("Bookmarks");
        AddBookmarkItems(&menu, snapshot, *snapshot, BMessenger(this));
        Check(menu.CountItems() == 5, "menu includes ordered separators, bookmark and Open All");
        Check(dynamic_cast<BSeparatorItem*>(menu.ItemAt(0)) && dynamic_cast<BSeparatorItem*>(menu.ItemAt(2)),
            "imported separators use native separator menu items");
        Check(!menu.ItemAt(0)->Message() && !menu.ItemAt(2)->Message(), "menu separators have no navigation message");
        Check(menu.ItemAt(1)->Message() && std::string(menu.ItemAt(1)->Message()->GetString("url", "")) == "https://example.com/",
            "bookmark between separators retains its address");
        auto onlySeparators = Snapshot();
        onlySeparators->children.erase(onlySeparators->children.begin() + 1);
        BMenu emptyMenu("Separators");
        AddBookmarkItems(&emptyMenu, onlySeparators, *onlySeparators, BMessenger(this));
        Check(emptyMenu.CountItems() == 2, "separator-only menu offers no Open All command");
        auto scripts = std::make_shared<BookmarkButton>();
        scripts->folder = true;
        BookmarkButton script;
        script.url = "javascript:void(window.menu=1)";
        script.title = "Bookmarklet";
        scripts->children.push_back(script);
        BMenu scriptMenu("Scripts");
        AddBookmarkItems(&scriptMenu, scripts, *scripts, BMessenger(this));
        Check(scriptMenu.CountItems() == 1 && scriptMenu.ItemAt(0)->Message()
            && scriptMenu.ItemAt(0)->Message()->what == kOpenBookmark, "bookmarklet-only folder has activation but no Open All");

        const auto historyFile = fDirectory / "History";
        std::string sqlite = "libsqlite3.so.0";
        if (const char* library = std::getenv("SUMMIT_TEST_SQLITE")) sqlite = library;
        Check(SQL(sqlite, historyFile, R"(
            CREATE TABLE urls(id INTEGER PRIMARY KEY,url TEXT,title TEXT,visit_count INTEGER,hidden INTEGER,last_visit_time INTEGER);
            CREATE TABLE visits(id INTEGER PRIMARY KEY,url INTEGER,visit_time INTEGER);
            INSERT INTO urls VALUES(1,'https://visits.test/','Imported',2,0,13344473600000123);
            INSERT INTO visits VALUES(1,1,13344473600000123),(2,1,13344470000000000);
        )"), "native history import fixture created");
        fShared->Change([](Profile& profile) -> uint32 {
            profile.Visit({"https://visits.test/", "Local"}, 1600000000);
            return SharedProfile::kHistoryChanged;
        });
        Check(fShared->Save(error), "initial history saved before repeat-page import");
        result = ImportBookmarksFrom(*fShared, historyFile, true);
        Check(result.ok && !result.history && result.message.find("Updated visits") != std::string::npos,
            "additional visits import reports an update without adding a URL");
        Check(fShared->Save(error), "history-only update can be saved");
        loaded = Profile::Load(profilePath, error);
        Check(error.empty() && loaded.history.size() == 1 && loaded.history.front().visitTimes.size() == 3
            && loaded.history.front().visitTimes.front() == 1700000000000123,
            "additional microsecond visits survive native import and profile reopen");
        result = ImportBookmarksFrom(*fShared, historyFile, true);
        Check(result.ok && !result.history, "repeating a history-only import is idempotent");

        Bookmarks queries;
        const auto popular = queries.Add({"place:sort=8&maxResults=2", "Most visited"}, kBookmarksBarFolder);
        std::vector<PageRecord> visits{{"https://one.test/", "One", 100, false, 1}, {"https://two.test/", "Two", 200, false, 8}};
        auto queryButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(queries, visits, queries.Bar()));
        Check(queryButtons->children[0].folder && bool(queryButtons->children[0].queryContents)
            && queryButtons->children[0].children.empty(), "smart folder keeps a lazy query in the native toolbar snapshot");
        BMenu popularMenu("Popular");
        AddBookmarkItems(&popularMenu, queryButtons, queryButtons->children[0], BMessenger(this));
        Check(popularMenu.CountItems() == 4 && std::string(popularMenu.ItemAt(0)->Label()) == "Two",
            "opening the native query menu evaluates visit order and includes Open All");
        Check(std::string(popularMenu.ItemAt(0)->Message()->GetString("url", "")) == "https://two.test/",
            "native query result carries the page address");
        Check(popularMenu.ItemAt(3)->Message()->GetInt64("id", 0) == popular,
            "Open All identifies the saved query for evaluation at activation");
        visits[0].visits = 30;
        auto updatedQueries = std::make_shared<const BookmarkButton>(BookmarkButtons(queries, visits, queries.Bar()));
        BMenu updatedMenu("Updated"), originalMenu("Original");
        AddBookmarkItems(&updatedMenu, updatedQueries, updatedQueries->children[0], BMessenger(this));
        AddBookmarkItems(&originalMenu, queryButtons, queryButtons->children[0], BMessenger(this));
        Check(std::string(updatedMenu.ItemAt(0)->Label()) == "One" && std::string(originalMenu.ItemAt(0)->Label()) == "Two",
            "updated history changes the next snapshot without mutating an open menu");
        const auto bad = queries.Add({"place:parent=unknown", "Folder shortcut"}, kBookmarksBarFolder);
        auto badButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(queries, visits, *queries.Find(bad)));
        BMenu badMenu("Unsupported");
        AddBookmarkItems(&badMenu, badButtons, *badButtons, BMessenger(this));
        Check(badMenu.CountItems() == 1 && !badMenu.ItemAt(0)->IsEnabled()
            && std::string(badMenu.ItemAt(0)->Label()).find("target is missing or was not imported") != std::string::npos,
            "unsupported query is retained as a disabled explanation instead of broadening its results");
        const auto self = queries.Add({"place:parent=toolbar_____", "Recursive"}, kBookmarksBarFolder);
        auto recursive = BookmarkButtons(queries, visits, *queries.Find(self)).queryContents();
        const auto nested = std::find_if(recursive.children.begin(), recursive.children.end(), [self](const auto& button) { return button.id == self; });
        Check(nested != recursive.children.end() && nested->queryContents().emptyMessage.find("circular") != std::string::npos,
            "native smart menus stop circular query expansion");
        Check(queries.Find(popular)->children.empty() && queries.Count() == 3,
            "native menu expansion does not save derived bookmarks");

        SetHistoryVisitTimes(visits[0], {300000000, 100000000});
        SetHistoryVisitTimes(visits[1], {200000000});
        const auto timed = queries.Add({"place:beginTime=99999999&endTime=100000000", "Earlier visit"}, kBookmarksBarFolder);
        auto timeButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(queries, visits, *queries.Find(timed)));
        BMenu timeMenu("Earlier visit");
        AddBookmarkItems(&timeMenu, timeButtons, *timeButtons, BMessenger(this));
        Check(timeMenu.CountItems() == 3 && std::string(timeMenu.ItemAt(0)->Label()) == "One",
            "native time query matches an earlier visit rather than the latest visit");
        Check(timeMenu.ItemAt(2)->Message()->GetInt64("id", 0) == timed,
            "time query Open All retains the saved query identity");
        SetHistoryVisitTimes(visits[0], {300000000});
        auto clearedTimes = BookmarkButtons(queries, visits, *queries.Find(timed)).queryContents();
        Check(clearedTimes.children.empty() && !timeButtons->queryContents().children.empty(),
            "time query changes with retained visits without changing an open snapshot");

        BookmarkNode dated;
        dated.children = {{0, "Dated A", "https://modified.test/a", false, 0, {}},
            {0, "Dated B", "https://modified.test/b", false, 0, {}}};
        dated.children[0].modified = 1000002;
        dated.children[1].modified = 1000001;
        Bookmarks modified;
        modified.Assign(std::move(dated), {});
        const auto modifiedQuery = modified.Add({"place:queryType=1&sort=14", "Recently modified"}, kBookmarksBarFolder);
        auto modifiedButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(modified, {}, *modified.Find(modifiedQuery)));
        BMenu modifiedMenu("Recently modified");
        AddBookmarkItems(&modifiedMenu, modifiedButtons, *modifiedButtons, BMessenger(this));
        Check(modifiedMenu.CountItems() == 4 && std::string(modifiedMenu.ItemAt(0)->Label()) == "Dated A",
            "native modification query uses precise dates and excludes itself");
        const auto datedB = modified.FindURL("https://modified.test/b")->id;
        Check(modified.Edit(datedB, "Changed B", "https://modified.test/b"), "editing updates the bookmark modification time");
        auto changedDates = BookmarkButtons(modified, {}, *modified.Find(modifiedQuery)).queryContents();
        Check(changedDates.children[0].title == "Changed B" && modifiedButtons->queryContents().children[0].title == "Dated A",
            "modification query reorder leaves an open native snapshot intact");
        const auto editTime = modified.Find(datedB)->modified;
        Check(!modified.Edit(datedB, "Changed B", "https://modified.test/b") && modified.Find(datedB)->modified == editTime,
            "saving an unchanged bookmark does not rewrite its modification date");

        const auto combinedQuery = modified.Add({"place:queryType=1&terms=Dated&OR&terms=Changed&sort=14&maxResults=1", "Combined"}, kBookmarksBarFolder);
        auto combinedButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(modified, {}, *modified.Find(combinedQuery)));
        BMenu combinedMenu("Combined");
        AddBookmarkItems(&combinedMenu, combinedButtons, *combinedButtons, BMessenger(this));
        Check(combinedMenu.CountItems() == 3 && std::string(combinedMenu.ItemAt(0)->Label()) == "Changed B",
            "combined native query applies sorting and limit across both branches");
        Check(combinedButtons->queryContents().children.size() == 1,
            "combined native query has one result after the global limit");
        const auto datedA = modified.FindURL("https://modified.test/a")->id;
        Check(modified.Edit(datedA, "Changed A", "https://modified.test/a"), "combined query fixture changes through native bookmark model");
        auto changedCombined = BookmarkButtons(modified, {}, *modified.Find(combinedQuery)).queryContents();
        Check(changedCombined.children[0].title == "Changed A" && combinedButtons->queryContents().children[0].title == "Changed B",
            "combined native query refresh preserves the already-open snapshot");
        Check(modified.Remove(datedA) && BookmarkButtons(modified, {}, *modified.Find(combinedQuery)).queryContents().children[0].title == "Changed B",
            "removing a match lets the other combined-query branch supply the result");

        const auto siteQuery = queries.Add({"place:type=4&sort=1", "Sites"}, kBookmarksBarFolder);
        visits.push_back({"https://one.test/second", "Another one", 400, false, 2});
        auto siteButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(queries, visits, *queries.Find(siteQuery)));
        BMenu siteMenu("Sites");
        AddBookmarkItems(&siteMenu, siteButtons, *siteButtons, BMessenger(this));
        Check(siteMenu.CountItems() == 2 && siteMenu.ItemAt(0)->Submenu() && siteMenu.ItemAt(1)->Submenu(),
            "site query exposes native host submenus without a root Open All");
        auto expandedSites = siteButtons->queryContents();
        BMenu hostItems("one.test");
        AddBookmarkItems(&hostItems, siteButtons, expandedSites.children[0], BMessenger(this));
        auto* hostMenu = &hostItems;
        Check(std::string(siteMenu.ItemAt(0)->Label()) == "one.test" && hostMenu->CountItems() == 4,
            "host submenu includes both history pages and Open All");
        if (hostMenu && hostMenu->CountItems() == 4) {
            Check(std::string(hostMenu->ItemAt(0)->Message()->GetString("url", "")) == "https://one.test/second",
                "site result carries its actual page URL in title order");
            const auto* all = hostMenu->ItemAt(3)->Message();
            Check(all && all->GetInt64("id", 0) == 0 && all->CountNames(B_STRING_TYPE) == 1
                && std::string(all->GetString("url", "")) == "https://one.test/second",
                "transient host Open All embeds URLs instead of a persisted folder ID");
        }
        visits.push_back({"https://new.test/", "New site", 500, false, 1});
        Check(BookmarkButtons(queries, visits, *queries.Find(siteQuery)).queryContents().children.size() == 3
            && siteButtons->queryContents().children.size() == 2,
            "site membership refresh leaves an open native menu snapshot intact");
        Check(queries.Find(siteQuery)->children.empty(), "native site expansion never persists generated folders");

        Bookmarks dates;
        const auto dateQuery = dates.Add({"place:type=3&sort=1", "Dates"}, kBookmarksBarFolder);
        const auto dateSiteQuery = dates.Add({"place:type=5&sort=4", "Dates and sites"}, kBookmarksBarFolder);
        std::vector<PageRecord> dateVisits{{"https://one.test/a", "A"}, {"https://two.test/b", "B"}};
        const auto clock = HistoryNowMicroseconds();
        for (auto& page : dateVisits) SetHistoryVisitTimes(page,{clock});
        auto dateButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(dates, dateVisits, *dates.Find(dateQuery)).queryContents());
        BMenu datesMenu("Dates");
        AddBookmarkItems(&datesMenu, dateButtons, *dateButtons, BMessenger(this));
        Check(datesMenu.CountItems() == 1 && datesMenu.ItemAt(0)->Submenu()
            && std::string(datesMenu.ItemAt(0)->Label()) == "Today", "date query has a native Today submenu");
        BMenu todayMenu("Today");
        AddBookmarkItems(&todayMenu, dateButtons, dateButtons->children[0], BMessenger(this));
        const auto* dateAll = todayMenu.ItemAt(todayMenu.CountItems()-1)->Message();
        const char* secondURL = nullptr;
        Check(todayMenu.CountItems() == 4 && dateAll && dateAll->GetInt64("id", -1) == 0
            && dateAll->FindString("url", 1, &secondURL) == B_OK && std::string(secondURL) == "https://two.test/b",
            "date folder Open All embeds both history URLs with no saved folder ID");
        auto dateSites = std::make_shared<const BookmarkButton>(BookmarkButtons(dates, dateVisits, *dates.Find(dateSiteQuery)).queryContents());
        BMenu todaySites("Today sites");
        AddBookmarkItems(&todaySites, dateSites, dateSites->children[0], BMessenger(this));
        Check(todaySites.CountItems() == 2 && todaySites.ItemAt(0)->Submenu() && todaySites.ItemAt(1)->Submenu(),
            "date/site query retains the intermediate native host submenus");
        BMenu oneSite("One site");
        AddBookmarkItems(&oneSite, dateSites, dateSites->children[0].children[0], BMessenger(this));
        Check(oneSite.CountItems() == 3 && std::string(oneSite.ItemAt(0)->Message()->GetString("url", "")) == "https://one.test/a",
            "nested date/site leaf retains its page address and Open All action");
        Check(BookmarkButtons(dates, {}, *dates.Find(dateQuery)).queryContents().children.empty()
            && dateButtons->children.size() == 1, "clearing history empties later date snapshots without changing an open menu");
        Check(dates.Count() == 2 && dates.Find(dateQuery)->children.empty() && dates.Find(dateSiteQuery)->children.empty(),
            "native date and host containers are never saved as bookmarks");

        Bookmarks individual;
        const auto individualQuery = individual.Add({"place:type=1&sort=4", "Visits"}, kBookmarksBarFolder);
        std::vector<PageRecord> repeated{{"https://visits.test/a", "Repeated", 0, false, 7}, {"https://visits.test/b", ""}};
        SetHistoryVisitTimes(repeated[0], {clock-1, clock-2});
        SetHistoryVisitTimes(repeated[1], {clock-3});
        auto visitSnapshot = BookmarkButtons(individual, repeated, *individual.Find(individualQuery));
        auto visitButtons = std::make_shared<const BookmarkButton>(visitSnapshot.queryContents());
        Check(visitButtons->children.size() == 3 && visitButtons->children[0].url == visitButtons->children[1].url,
            "individual query retains repeated visits without inventing aggregate-count rows");
        Check(visitButtons->children[0].title == "Repeated — " + FormatBookmarkVisitTime(clock-1)
            && visitButtons->children[1].title == "Repeated — " + FormatBookmarkVisitTime(clock-2),
            "native visit labels distinguish subsecond timestamps");
        Check(visitButtons->children[2].title == "https://visits.test/b — " + FormatBookmarkVisitTime(clock-3),
            "untitled visits retain a readable address before their timestamp");
        BMenu visitMenu("Visits");
        AddBookmarkItems(&visitMenu, visitButtons, *visitButtons, BMessenger(this));
        Check(visitMenu.CountItems() == 5 && std::string(visitMenu.ItemAt(0)->Label()) != visitMenu.ItemAt(1)->Label(),
            "native menu displays distinguishable repeated visits and Open All");
        Check(std::string(visitMenu.ItemAt(0)->Message()->GetString("url", "")) == "https://visits.test/a"
            && std::string(visitMenu.ItemAt(1)->Message()->GetString("url", "")) == "https://visits.test/a",
            "both visit actions navigate to the original page address");
        Check(visitMenu.ItemAt(4)->Message()->GetInt64("id", -1) == individualQuery,
            "visit Open All resolves the saved query at activation");
        repeated.clear();
        Check(BookmarkButtons(individual, repeated, *individual.Find(individualQuery)).queryContents().children.empty()
            && visitSnapshot.queryContents().children.size() == 3, "open visit menus retain their immutable history snapshot");
        Check(individual.Count() == 1 && individual.Find(individualQuery)->children.empty()
            && individual.Find(individualQuery)->title == "Visits", "visit timestamps never alter saved bookmark data");

        Bookmarks roots;
        BookmarkNode rootsImport{0, "Roots", "", true, 0, {}};
        rootsImport.origin = {"native-roots", "root________", 1};
        for (const auto* guid : {"toolbar_____", "menu________", "unfiled_____", "mobile______"}) {
            BookmarkNode folder{0, guid, "", true, 0, {}};
            folder.origin = {"native-roots", guid, int64_t(rootsImport.children.size()+2)};
            rootsImport.children.push_back(std::move(folder));
        }
        auto& rootToolbar = rootsImport.children[0];
        rootToolbar.children = {{0, "Z page", "https://roots.test/z", false, 0, {}},
            {0, "A page", "https://roots.test/a", false, 0, {}},
            {0, "History query", "place:sort=4", false, 0, {}},
            {0, "Folder shortcut", "place:parent=unfiled_____", false, 0, {}}};
        for (size_t i=0; i<rootToolbar.children.size(); ++i)
            rootToolbar.children[i].origin = {"native-roots", "", int64_t(i+10)};
        BookmarkNode rootsQuery{0, "Root query", "place:type=8&sort=2", false, 0, {}};
        rootsQuery.origin = {"native-roots", "root_query__", 90};
        rootsImport.children[1].children.push_back(rootsQuery);
        Check(roots.Import(rootsImport, "Native roots").Changed(), "native root fixture imports source identities");
        const auto rootQueryID = roots.FindURL(rootsQuery.url)->id;
        auto rootButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(roots, {}, *roots.Find(rootQueryID)).queryContents());
        Check(rootButtons->children.size()==3 && rootButtons->children[0].title=="Other Bookmarks"
            && rootButtons->children[1].title=="Bookmarks Toolbar", "virtual roots use localized root titles and descending order");
        BMenu rootMenu("Roots"); AddBookmarkItems(&rootMenu, rootButtons, *rootButtons, BMessenger(this));
        Check(rootMenu.CountItems()==3 && rootMenu.ItemAt(1)->Submenu(), "native root result exposes real submenus");
        BMenu rootPages("Root pages"); AddBookmarkItems(&rootPages, rootButtons, rootButtons->children[1], BMessenger(this));
        const auto* rootsAll = rootPages.ItemAt(rootPages.CountItems()-1)->Message();
        const char* firstRootURL=nullptr;
        Check(rootsAll && rootsAll->GetInt64("id",-1)==0 && rootsAll->FindString("url",0,&firstRootURL)==B_OK
            && std::string(firstRootURL)=="https://roots.test/z", "root Open All embeds displayed URLs instead of a persistent folder ID");
        BMenu emptyRoot("Other"); AddBookmarkItems(&emptyRoot, rootButtons, rootButtons->children[0], BMessenger(this));
        Check(emptyRoot.CountItems()==1 && !emptyRoot.ItemAt(0)->IsEnabled(), "empty virtual roots remain visible as empty submenus");
        Check(roots.Edit(rootQueryID,"Root query","place:type=8&expandQueries=0"), "native root query can disable nested query expansion");
        auto unexpandedRoots = BookmarkButtons(roots, {}, *roots.Find(rootQueryID)).queryContents();
        const auto& historyLink = unexpandedRoots.children[0].children[2];
        const auto& folderLink = unexpandedRoots.children[0].children[3];
        Check(!historyLink.folder && !historyLink.queryContents && historyLink.url=="place:sort=4",
            "disabled saved-query expansion leaves a normal native action");
        Check(folderLink.folder && folderLink.queryContents && folderLink.queryContents().children.empty(),
            "folder shortcuts still expand with expandQueries disabled");
        Check(roots.Edit(rootQueryID,"Root query","place:type=8&excludeItems=1&excludeQueries=1"), "native root exclusions can be edited");
        auto excludedRoots = BookmarkButtons(roots, {}, *roots.Find(rootQueryID)).queryContents();
        Check(excludedRoots.children[0].children.size()==1 && excludedRoots.children[0].children[0].title=="Folder shortcut",
            "root exclusions hide pages and searches but preserve folder links");
        Check(rootButtons->children[1].children.size()==4 && roots.Find(rootQueryID)->children.empty(),
            "root snapshots remain immutable and never replace the stored query");
        Check(rootButtons->children[2].children[0].queryContents().emptyMessage.find("circular")!=std::string::npos,
            "a root view containing its own saved query stops circular native expansion");
        const auto unfiledRootID = roots.Other().children[0].children[2].id;
        Check(roots.Add({"place:sort=8", "Nested history"},unfiledRootID)>0,
            "nested saved query can be added to the shortcut target");
        Check(roots.Edit(rootQueryID,"Root query","place:type=8&expandQueries=0"),
            "root query can restore disabled expansion after exclusion checks");
        const auto nestedExpansion = BookmarkButtons(roots, {}, *roots.Find(rootQueryID)).queryContents()
            .children[0].children[3].queryContents();
        Check(nestedExpansion.children.size()==1 && !nestedExpansion.children[0].folder && !nestedExpansion.children[0].queryContents,
            "disabled query expansion propagates through an expandable folder shortcut");

        Bookmarks scoped;
        for (const auto* scope : {"first", "second"}) {
            BookmarkNode imported{0, "", "", true, 0, {}};
            imported.origin = {scope, "root________", 1};
            BookmarkNode folder{0, "Work", "", true, 0, {}};
            folder.origin = {scope, "work________", 10};
            folder.children.push_back({0, scope, std::string("https://") + scope + ".test/", false, 0, {}});
            folder.children.back().origin = {scope, "page________", 20};
            folder.children.push_back({0, "", "", false, 0, {}, true});
            folder.children.back().origin = {scope, "separator___", 21};
            imported.children.push_back(folder);
            imported.children.push_back({0, scope, "place:parent=work________", false, 0, {}});
            imported.children.back().origin = {scope, "query_______", 30};
            Check(scoped.Import(imported, "Firefox").added == 2, "scoped native query fixture imports");
        }
        const auto& secondQuery = scoped.Other().children.back().children.back();
        auto scopedButtons = std::make_shared<const BookmarkButton>(BookmarkButtons(scoped, {}, secondQuery));
        BMenu scopedMenu("Scoped");
        AddBookmarkItems(&scopedMenu, scopedButtons, *scopedButtons, BMessenger(this));
        Check(scopedMenu.CountItems() == 4 && std::string(scopedMenu.ItemAt(0)->Label()) == "second",
            "same query URL in two imported profiles expands the correct native folder");
        Check(std::string(scopedMenu.ItemAt(0)->Message()->GetString("url", "")) == "https://second.test/",
            "scoped query menu result retains its correct address");
        Check(dynamic_cast<BSeparatorItem*>(scopedMenu.ItemAt(1)), "folder query preserves native separators");
        Check(scopedMenu.ItemAt(3)->Message()->GetInt64("id", 0) == secondQuery.id,
            "scoped Open All identifies the actual saved query");

        BBitmap bitmap(BRect(0, 0, 399, 27), B_RGB32, true);
        const bool locked = bitmap.InitCheck() == B_OK && bitmap.Lock();
        Check(locked, "native toolbar bitmap is available");
        if (!locked) { PostMessage(B_QUIT_REQUESTED); return; }
        auto* bar = new BookmarksBar;
        bitmap.AddChild(bar);
        bar->ResizeTo(399, 27);
        bar->SetBookmarks(snapshot);
        bar->Draw(bar->Bounds());
        bar->Sync();
        auto* pixels = static_cast<const uint32*>(bitmap.Bits());
        const int32 stride = bitmap.BytesPerRow() / 4;
        Check(pixels[10 * stride + 11] != pixels[10 * stride + 8], "toolbar separator draws a vertical line");
        Check(pixels[4 * stride + 11] == pixels[4 * stride + 8], "toolbar separator leaves space above the line");
        bitmap.Unlock();
        fWindow = BMessenger(new TestWindow);
        fStarted = system_time();
        fWindow.SendMessage(kExercise);
        SetPulseRate(100000);
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kProfileChanged) {
            if (message->GetUInt32("changes", 0) & SharedProfile::kBookmarksChanged) ++fNotifications;
            if (message->GetUInt32("changes", 0) & SharedProfile::kHistoryChanged) ++fHistoryNotifications;
        } else if (message->what == kBookmarkEdited) {
            Check(std::string(message->GetString("url", "")) == "JaVaScRiPt:void(document.title='Edited')",
                "native editor saves bookmarklet source without adding https");
            Check(std::string(message->GetString("keywords", "")) == "search, alias", "native Save carries edited shortcut names");
            Check(std::string(message->GetString("description", "")) == "Edited\nsecond line",
                "native Save retains the edited multiline description");
            std::vector<std::string> tags;
            const char* tag;
            for (int32 i = 0; message->FindString("tag", i, &tag) == B_OK; ++i) tags.emplace_back(tag);
            Check(tags == std::vector<std::string>{"one", "comma, tag", "quote\"tag"}, "native Save retains quoted tag names");
            fEditorDone = true;
        } else if (message->what == kDone) {
            Check(message->GetInt32("opened", -1) == 1 && std::string(message->GetString("url", "")) == "https://example.com/",
                "toolbar click ignores separator and opens the adjacent bookmark once");
            fDone = true;
        } else BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (fDone && !fWindow.IsValid() && fEditorDone && !fEditor.IsValid()) {
            Check(fNotifications == 5, "only changed imports notify the browser windows");
            Check(fHistoryNotifications == 2, "additional visits notify windows once; identical reimport does not");
            fShared.reset();
            std::printf("%d native checks, %d failures\n", checks, failures);
            PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - fStarted > 5000000) {
            Check(false, "native test finishes within five seconds");
            if (fWindow.IsValid()) fWindow.SendMessage(B_QUIT_REQUESTED);
            if (fEditor.IsValid()) fEditor.SendMessage(B_QUIT_REQUESTED);
            PostMessage(B_QUIT_REQUESTED);
        }
    }
private:
    std::filesystem::path fDirectory;
    std::unique_ptr<SharedProfile> fShared;
    BMessenger fWindow;
    BMessenger fEditor;
    int fNotifications = 0;
    int fHistoryNotifications = 0;
    bool fDone = false;
    bool fEditorDone = false;
    bigtime_t fStarted = 0;
};
}
int main(int argc, char** argv)
{
    if (argc != 2) return 64;
    Tests app(argv[1]);
    app.Run();
    return failures ? 1 : 0;
}
